#!/usr/bin/env python3
"""
Standalone Serial Logger — Serial data capture with optional packet decoding.

Logs raw serial data to timestamped files. Optionally decodes structured
packets when a packet_formats.json config file is present. Works with any
serial device (radio receivers, UART bridges, debug consoles, etc.).
"""

import json
import os
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial is required: pip install pyserial")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Packet Decoder
# ---------------------------------------------------------------------------

class PacketDecoder:
    """Stateful packet decoder driven by a JSON format definition."""

    def __init__(self, config_path=None):
        self.format_packets = {}   # header_byte -> {name, length, fields}
        self.hybrid_rules = []     # [{match, length, name, ...}]
        self.hybrid_packets = {}   # name -> {length, fields}
        self.checksum_type = None
        self.mode = "format"       # "format", "hybrid", or "raw"
        self.loaded = False

        # Parser state
        self._buf = bytearray()
        self._pkt_expected = 0
        self._pkt_name = ""

        if config_path and os.path.isfile(config_path):
            self._load(config_path)

    def _load(self, path):
        try:
            with open(path, 'r') as f:
                cfg = json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            print("Warning: could not load {}: {}".format(path, e))
            return

        # Checksum
        cs = cfg.get("checksum", {})
        self.checksum_type = cs.get("type")  # "xor" or None

        # Format mode packets
        fm = cfg.get("format_mode", {})
        for pkt in fm.get("packets", []):
            hdr = int(pkt["header"], 16)
            self.format_packets[hdr] = {
                "name": pkt["name"],
                "length": pkt["length"],
                "fields": pkt.get("fields", []),
            }

        # Hybrid mode
        hm = cfg.get("hybrid_mode", {})
        self.hybrid_rules = hm.get("framing_rules", [])
        for pkt in hm.get("packets", []):
            self.hybrid_packets[pkt["name"]] = {
                "length": pkt["length"],
                "fields": pkt.get("fields", []),
            }

        self.loaded = bool(self.format_packets or self.hybrid_rules)

    def set_mode(self, mode):
        """Set decode mode: 'format', 'hybrid', or 'raw'."""
        self.mode = mode
        self.reset()

    def reset(self):
        self._buf.clear()
        self._pkt_expected = 0
        self._pkt_name = ""

    def feed(self, data):
        """Feed bytes, return list of decoded packet dicts."""
        if self.mode == "raw" or not self.loaded:
            return []
        results = []
        for b in data:
            pkts = self._feed_byte(b)
            results.extend(pkts)
        return results

    def _feed_byte(self, b):
        if self.mode == "format":
            return self._feed_format(b)
        elif self.mode == "hybrid":
            return self._feed_hybrid(b)
        return []

    def _feed_format(self, b):
        if self._pkt_expected == 0:
            # Waiting for header
            info = self.format_packets.get(b)
            if info:
                self._buf.clear()
                self._buf.append(b)
                self._pkt_expected = info["length"]
                self._pkt_name = info["name"]
            return []
        else:
            self._buf.append(b)
            if len(self._buf) >= self._pkt_expected:
                pkt = self._decode_format_packet()
                self._pkt_expected = 0
                self._pkt_name = ""
                if pkt:
                    return [pkt]
            return []

    def _feed_hybrid(self, b):
        if self._pkt_expected == 0:
            # Determine packet type from first byte
            length, name = self._hybrid_match(b)
            if length == 0:
                return []  # unknown byte, skip
            self._buf.clear()
            self._buf.append(b)
            self._pkt_expected = length
            self._pkt_name = name
            if length == 1:
                pkt = self._decode_hybrid_packet()
                self._pkt_expected = 0
                return [pkt] if pkt else []
            return []
        else:
            self._buf.append(b)
            if len(self._buf) >= self._pkt_expected:
                pkt = self._decode_hybrid_packet()
                self._pkt_expected = 0
                self._pkt_name = ""
                if pkt:
                    return [pkt]
            return []

    def _hybrid_match(self, b):
        for rule in self.hybrid_rules:
            if rule["match"] == "value":
                val = int(rule["value"], 16)
                if b == val:
                    return rule["length"], rule["name"]
            elif rule["match"] == "bitmask":
                mask = int(rule["mask"], 16)
                expect = int(rule["expect"], 16)
                if (b & mask) == expect:
                    return rule["length"], rule["name"]
        return 0, ""

    def _decode_format_packet(self):
        raw = bytes(self._buf)
        info = self.format_packets.get(raw[0])
        if not info:
            return None

        # Checksum validation
        valid = True
        if self.checksum_type == "xor":
            xor = 0
            for byte in raw:
                xor ^= byte
            valid = (xor == 0)

        fields = self._decode_fields(raw, info["fields"])
        return {
            "name": info["name"],
            "raw": raw,
            "valid": valid,
            "fields": fields,
        }

    def _decode_hybrid_packet(self):
        raw = bytes(self._buf)
        info = self.hybrid_packets.get(self._pkt_name)
        field_defs = info["fields"] if info else []
        fields = self._decode_fields(raw, field_defs)
        return {
            "name": self._pkt_name,
            "raw": raw,
            "valid": True,  # hybrid has no per-packet checksum
            "fields": fields,
        }

    def _decode_fields(self, raw, field_defs):
        fields = {}
        for fd in field_defs:
            name = fd["name"]
            offset = fd["offset"]
            size = fd["size"]
            fmt = fd.get("format", "hex")

            if offset + size > len(raw):
                fields[name] = "?"
                continue

            chunk = raw[offset:offset + size]

            if fmt == "hex":
                fields[name] = ' '.join('{:02X}'.format(x) for x in chunk)
            elif fmt == "uint8":
                fields[name] = chunk[0]
            elif fmt == "uint16":
                fields[name] = int.from_bytes(chunk, 'big')
            elif fmt == "uint32":
                fields[name] = int.from_bytes(chunk, 'big')
            elif fmt == "bin":
                fields[name] = ''.join('{:08b}'.format(x) for x in chunk)
            elif fmt == "bit":
                bit_num = fd.get("bit", 0)
                fields[name] = (chunk[0] >> bit_num) & 1
            elif fmt == "mask":
                mask = int(fd.get("mask", "0xFF"), 16)
                fields[name] = chunk[0] & mask
            elif fmt == "bits":
                bit_defs = fd.get("bits", {})
                active = []
                for bit_str, label in bit_defs.items():
                    if chunk[0] & (1 << int(bit_str)):
                        active.append(label)
                fields[name] = ', '.join(active) if active else "none"
            elif fmt == "enum":
                values = fd.get("values", {})
                fields[name] = values.get(str(chunk[0]), "unknown({})".format(chunk[0]))
            else:
                fields[name] = ' '.join('{:02X}'.format(x) for x in chunk)

            if "unit" in fd and isinstance(fields[name], (int, float)):
                fields[name] = "{} {}".format(fields[name], fd["unit"])

        return fields


def format_packet(pkt):
    """Format a decoded packet dict into a human-readable string."""
    cs = "OK" if pkt["valid"] else "BAD"
    raw_hex = ' '.join('{:02X}'.format(b) for b in pkt["raw"])
    parts = ["[{}] [{}]".format(pkt["name"], cs)]
    for k, v in pkt["fields"].items():
        if k in ("header", "checksum", "marker"):
            continue
        parts.append("{}={}".format(k, v))
    parts.append("({})".format(raw_hex))
    return ' '.join(parts)


# ---------------------------------------------------------------------------
# Serial Logger (threaded reader)
# ---------------------------------------------------------------------------

class SerialLogger:
    """Threaded serial reader with file logging."""

    def __init__(self):
        self.port = None
        self._lock = threading.Lock()
        self._running = False
        self._thread = None
        self._log_file = None
        self._logging = False
        self.on_data = None       # callback(bytes)
        self.on_disconnect = None  # callback()
        self.bytes_logged = 0

    def connect(self, port_name, baudrate=115200):
        with self._lock:
            if self.port and self.port.is_open:
                self.port.close()
            self.port = serial.Serial()
            self.port.port = port_name
            self.port.baudrate = baudrate
            self.port.bytesize = serial.EIGHTBITS
            self.port.parity = serial.PARITY_NONE
            self.port.stopbits = serial.STOPBITS_ONE
            self.port.timeout = 0.05
            self.port.open()
            self._running = True
            self._thread = threading.Thread(target=self._read_loop, daemon=True)
            self._thread.start()
            return self.port.name

    def disconnect(self):
        self._running = False
        self.stop_logging()
        with self._lock:
            if self.port and self.port.is_open:
                self.port.close()
            self.port = None

    @property
    def connected(self):
        with self._lock:
            return self.port is not None and self.port.is_open

    def start_logging(self, filepath):
        self.stop_logging()
        self._log_file = open(filepath, "wb")
        self.bytes_logged = 0
        self._logging = True

    def stop_logging(self):
        self._logging = False
        if self._log_file:
            self._log_file.close()
            self._log_file = None

    def _read_loop(self):
        while self._running:
            try:
                with self._lock:
                    if not self.port or not self.port.is_open:
                        break
                    port = self.port
                waiting = port.in_waiting
                data = port.read(max(waiting, 1))
                if data:
                    if self._logging and self._log_file:
                        self._log_file.write(data)
                        self._log_file.flush()
                        self.bytes_logged += len(data)
                    if self.on_data:
                        self.on_data(data)
            except serial.SerialException:
                self._running = False
                if self.on_disconnect:
                    self.on_disconnect()
                break
            except Exception:
                pass


# ---------------------------------------------------------------------------
# GUI Application
# ---------------------------------------------------------------------------

class SerialLoggerApp:
    """Standalone serial logger GUI with optional packet decoding."""

    BAUD_RATES = ["9600", "19200", "38400", "57600", "115200",
                  "230400", "460800", "921600", "1000000", "5000000"]
    DISPLAY_HEX = "Hex"
    DISPLAY_ASCII = "ASCII"
    DISPLAY_BOTH = "Both"
    DISPLAY_DECODED = "Decoded"

    def __init__(self, root):
        self.root = root
        self.root.title("Serial Logger")
        self.root.geometry("850x600")
        self.root.minsize(700, 450)

        self.serial = SerialLogger()
        self.serial.on_data = self._on_data
        self.serial.on_disconnect = self._on_disconnect

        # Load packet decoder
        app_dir = os.path.dirname(os.path.abspath(__file__))
        config_path = os.path.join(app_dir, "packet_formats.json")
        self.decoder = PacketDecoder(config_path)
        self._decode_mode = "format"  # default decode mode
        self._packet_count = 0
        self._bad_checksum_count = 0

        self._port_map = {}
        self._log_active = False
        self._log_path = None
        self._hex_log_mode = False

        self._build_ui()
        self._refresh_ports()
        self._update_filename_preview()
        self._update_decode_controls()

        # Periodic UI update
        self._poll_stats()

    def _build_ui(self):
        main = ttk.Frame(self.root, padding=8)
        main.pack(fill=tk.BOTH, expand=True)

        # --- Connection frame ---
        conn = ttk.LabelFrame(main, text="Connection", padding=5)
        conn.pack(fill=tk.X, pady=(0, 5))

        ttk.Label(conn, text="Port:").pack(side=tk.LEFT)
        self._port_var = tk.StringVar()
        self._port_combo = ttk.Combobox(conn, textvariable=self._port_var, width=28, state="readonly")
        self._port_combo.pack(side=tk.LEFT, padx=(2, 5))

        ttk.Label(conn, text="Baud:").pack(side=tk.LEFT)
        self._baud_var = tk.StringVar(value="115200")
        self._baud_combo = ttk.Combobox(conn, textvariable=self._baud_var, width=9,
                                         values=self.BAUD_RATES, state="readonly")
        self._baud_combo.pack(side=tk.LEFT, padx=(2, 5))

        ttk.Button(conn, text="Refresh", command=self._refresh_ports).pack(side=tk.LEFT, padx=2)
        self._connect_btn = ttk.Button(conn, text="Connect", command=self._toggle_connect)
        self._connect_btn.pack(side=tk.LEFT, padx=2)

        self._conn_indicator = tk.Label(conn, text=" \u25cf ", font=("Arial", 14), fg="red")
        self._conn_indicator.pack(side=tk.LEFT, padx=5)

        # --- Logging frame ---
        log_frame = ttk.LabelFrame(main, text="Log File", padding=5)
        log_frame.pack(fill=tk.X, pady=(0, 5))

        row1 = ttk.Frame(log_frame)
        row1.pack(fill=tk.X, pady=(0, 3))

        ttk.Label(row1, text="Path:").pack(side=tk.LEFT)
        self._path_var = tk.StringVar(value=os.path.expanduser("~/"))
        self._path_entry = ttk.Entry(row1, textvariable=self._path_var, width=40)
        self._path_entry.pack(side=tk.LEFT, padx=(2, 2), fill=tk.X, expand=True)
        self._path_var.trace_add("write", lambda *_: self._update_filename_preview())
        ttk.Button(row1, text="Browse", command=self._browse_path).pack(side=tk.LEFT, padx=2)

        row2 = ttk.Frame(log_frame)
        row2.pack(fill=tk.X, pady=(0, 3))

        ttk.Label(row2, text="Prefix:").pack(side=tk.LEFT)
        self._prefix_var = tk.StringVar(value="serial_log")
        self._prefix_entry = ttk.Entry(row2, textvariable=self._prefix_var, width=20)
        self._prefix_entry.pack(side=tk.LEFT, padx=(2, 10))
        self._prefix_var.trace_add("write", lambda *_: self._update_filename_preview())

        ttk.Label(row2, text="Format:").pack(side=tk.LEFT)
        self._format_var = tk.StringVar(value="binary")
        ttk.Radiobutton(row2, text="Binary (.bin)", variable=self._format_var,
                        value="binary", command=self._update_filename_preview).pack(side=tk.LEFT, padx=2)
        ttk.Radiobutton(row2, text="Hex text (.log)", variable=self._format_var,
                        value="hex", command=self._update_filename_preview).pack(side=tk.LEFT, padx=2)

        row3 = ttk.Frame(log_frame)
        row3.pack(fill=tk.X, pady=(0, 3))

        ttk.Label(row3, text="File:").pack(side=tk.LEFT)
        self._filename_label = ttk.Label(row3, text="", foreground="gray")
        self._filename_label.pack(side=tk.LEFT, padx=(2, 10), fill=tk.X, expand=True)

        self._log_btn = ttk.Button(row3, text="Start Logging", command=self._toggle_logging)
        self._log_btn.pack(side=tk.RIGHT, padx=2)

        self._stats_label = ttk.Label(row3, text="")
        self._stats_label.pack(side=tk.RIGHT, padx=5)

        # --- Display frame ---
        disp_frame = ttk.LabelFrame(main, text="Received Data", padding=5)
        disp_frame.pack(fill=tk.BOTH, expand=True)

        disp_toolbar = ttk.Frame(disp_frame)
        disp_toolbar.pack(fill=tk.X, pady=(0, 3))

        ttk.Label(disp_toolbar, text="Display:").pack(side=tk.LEFT)
        self._display_var = tk.StringVar(value=self.DISPLAY_BOTH)
        modes = [self.DISPLAY_HEX, self.DISPLAY_ASCII, self.DISPLAY_BOTH]
        if self.decoder.loaded:
            modes.append(self.DISPLAY_DECODED)
        for mode in modes:
            ttk.Radiobutton(disp_toolbar, text=mode, variable=self._display_var,
                            value=mode, command=self._on_display_mode_change).pack(side=tk.LEFT, padx=2)

        # Decode mode selector (only visible when Decoded is active)
        self._decode_frame = ttk.Frame(disp_toolbar)
        ttk.Label(self._decode_frame, text="  Decode:").pack(side=tk.LEFT)
        self._decode_var = tk.StringVar(value="format")
        ttk.Radiobutton(self._decode_frame, text="Format", variable=self._decode_var,
                        value="format", command=self._on_decode_mode_change).pack(side=tk.LEFT, padx=2)
        ttk.Radiobutton(self._decode_frame, text="Hybrid", variable=self._decode_var,
                        value="hybrid", command=self._on_decode_mode_change).pack(side=tk.LEFT, padx=2)
        self._pkt_stats_label = ttk.Label(self._decode_frame, text="")
        self._pkt_stats_label.pack(side=tk.LEFT, padx=(10, 0))

        self._autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(disp_toolbar, text="Auto-scroll", variable=self._autoscroll_var).pack(side=tk.LEFT, padx=10)
        ttk.Button(disp_toolbar, text="Clear", command=self._clear_display).pack(side=tk.RIGHT, padx=2)

        self._text = tk.Text(disp_frame, wrap=tk.WORD, font=("Consolas", 9), height=15,
                             state=tk.DISABLED, bg="#1e1e1e", fg="#d4d4d4",
                             insertbackground="#d4d4d4", selectbackground="#264f78")
        self._text.tag_configure("pkt_name", foreground="#569cd6")
        self._text.tag_configure("pkt_ok", foreground="#6a9955")
        self._text.tag_configure("pkt_bad", foreground="#f44747")
        self._text.tag_configure("pkt_raw", foreground="#808080")
        self._text.tag_configure("pkt_field", foreground="#dcdcaa")
        scroll = ttk.Scrollbar(disp_frame, orient=tk.VERTICAL, command=self._text.yview)
        self._text.config(yscrollcommand=scroll.set)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self._text.pack(fill=tk.BOTH, expand=True)

        # Status bar
        self._status = ttk.Label(main, text="Disconnected", relief=tk.SUNKEN, anchor=tk.W)
        self._status.pack(fill=tk.X, pady=(5, 0))

    def _update_decode_controls(self):
        if self._display_var.get() == self.DISPLAY_DECODED and self.decoder.loaded:
            self._decode_frame.pack(side=tk.LEFT)
        else:
            self._decode_frame.pack_forget()

    def _on_display_mode_change(self):
        self._update_decode_controls()
        if self._display_var.get() == self.DISPLAY_DECODED:
            self.decoder.set_mode(self._decode_var.get())
            self._packet_count = 0
            self._bad_checksum_count = 0

    def _on_decode_mode_change(self):
        self.decoder.set_mode(self._decode_var.get())
        self._packet_count = 0
        self._bad_checksum_count = 0

    def _refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        self._port_map = {}
        descriptions = []
        for p in sorted(ports, key=lambda x: x.device):
            desc = "{} — {}".format(p.device, p.description)
            descriptions.append(desc)
            self._port_map[desc] = p.device
        self._port_combo["values"] = descriptions
        if descriptions:
            self._port_combo.current(0)

    def _toggle_connect(self):
        if self.serial.connected:
            self.serial.disconnect()
            self._connect_btn.config(text="Connect")
            self._conn_indicator.config(fg="red")
            self._status.config(text="Disconnected")
        else:
            sel = self._port_var.get()
            if not sel:
                messagebox.showwarning("No Port", "Select a port first.")
                return
            port = self._port_map.get(sel, sel)
            try:
                baud = int(self._baud_var.get())
                name = self.serial.connect(port, baudrate=baud)
                self._connect_btn.config(text="Disconnect")
                self._conn_indicator.config(fg="#00cc44")
                self._status.config(text="Connected: {} @ {} baud".format(name, baud))
                self.decoder.reset()
                self._packet_count = 0
                self._bad_checksum_count = 0
            except serial.SerialException as e:
                messagebox.showerror("Connection Error", str(e))

    def _browse_path(self):
        d = filedialog.askdirectory(initialdir=self._path_var.get())
        if d:
            self._path_var.set(d + "/")

    def _update_filename_preview(self):
        prefix = self._prefix_var.get() or "serial_log"
        ext = ".bin" if self._format_var.get() == "binary" else ".log"
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        name = "{}_{}{}".format(prefix, ts, ext)
        path = os.path.join(self._path_var.get(), name)
        self._filename_label.config(text=path)

    def _get_log_filepath(self):
        prefix = self._prefix_var.get() or "serial_log"
        ext = ".bin" if self._format_var.get() == "binary" else ".log"
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        name = "{}_{}{}".format(prefix, ts, ext)
        return os.path.join(self._path_var.get(), name)

    def _toggle_logging(self):
        if self._log_active:
            self.serial.stop_logging()
            self._log_active = False
            self._log_btn.config(text="Start Logging")
            self._append_text("\n--- Logging stopped ({} bytes) ---\n".format(
                self.serial.bytes_logged))
            self._status.config(text="Logging stopped. File: {}".format(self._log_path))
        else:
            if not self.serial.connected:
                messagebox.showwarning("Not Connected", "Connect to a serial port first.")
                return
            self._log_path = self._get_log_filepath()
            log_dir = os.path.dirname(self._log_path)
            if log_dir and not os.path.isdir(log_dir):
                messagebox.showerror("Invalid Path", "Directory does not exist:\n{}".format(log_dir))
                return
            if self._format_var.get() == "hex":
                self.serial.start_logging(self._log_path)
                # Override to write hex instead of raw binary
                self.serial._log_file.close()
                self.serial._log_file = open(self._log_path, "w")
                self.serial._log_file.write("# Serial Log: {}\n".format(
                    datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
                self.serial._log_file.write("# Port: {}, Baud: {}\n".format(
                    self._port_var.get(), self._baud_var.get()))
                self.serial._log_file.write("#\n")
                self._hex_log_mode = True
            else:
                self.serial.start_logging(self._log_path)
                self._hex_log_mode = False
            self._log_active = True
            self._log_btn.config(text="Stop Logging")
            self._append_text("\n--- Logging to {} ---\n".format(self._log_path))
            self._update_filename_preview()

    def _on_data(self, data):
        # Write hex to log file if in hex mode
        if self._log_active and self._hex_log_mode:
            if self.serial._log_file:
                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                hex_str = data.hex(' ')
                self.serial._log_file.write("[{}] {}\n".format(ts, hex_str))
                self.serial._log_file.flush()
                self.serial.bytes_logged += len(data)

        # Build display text
        mode = self._display_var.get()

        if mode == self.DISPLAY_DECODED and self.decoder.loaded:
            packets = self.decoder.feed(data)
            if packets:
                for pkt in packets:
                    self._packet_count += 1
                    if not pkt["valid"]:
                        self._bad_checksum_count += 1
                    self.root.after(0, lambda p=pkt: self._append_decoded(p))
            return

        if mode == self.DISPLAY_HEX:
            text = data.hex(' ') + ' '
        elif mode == self.DISPLAY_ASCII:
            text = data.decode('ascii', errors='replace')
        else:
            # Both: hex line then ASCII printable
            hex_part = data.hex(' ')
            ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)
            text = "{} |{}|\n".format(hex_part, ascii_part)

        self.root.after(0, lambda t=text: self._append_text(t))

    def _append_decoded(self, pkt):
        """Append a decoded packet to the display with color tags."""
        self._text.config(state=tk.NORMAL)

        # Packet name
        cs_tag = "pkt_ok" if pkt["valid"] else "pkt_bad"
        cs_text = "OK" if pkt["valid"] else "BAD CHECKSUM"

        self._text.insert(tk.END, "[{}]".format(pkt["name"]), "pkt_name")
        self._text.insert(tk.END, " [{}]".format(cs_text), cs_tag)

        # Fields (skip header, checksum, marker)
        for k, v in pkt["fields"].items():
            if k in ("header", "checksum", "marker"):
                continue
            self._text.insert(tk.END, " {}=".format(k))
            self._text.insert(tk.END, str(v), "pkt_field")

        # Raw hex
        raw_hex = ' '.join('{:02X}'.format(b) for b in pkt["raw"])
        self._text.insert(tk.END, " ({})".format(raw_hex), "pkt_raw")
        self._text.insert(tk.END, "\n")

        # Trim
        lines = int(self._text.index('end-1c').split('.')[0])
        if lines > 5000:
            self._text.delete('1.0', '{}.0'.format(lines - 5000))
        if self._autoscroll_var.get():
            self._text.see(tk.END)
        self._text.config(state=tk.DISABLED)

    def _on_disconnect(self):
        self.root.after(0, self._handle_disconnect)

    def _handle_disconnect(self):
        if self._log_active:
            self._toggle_logging()
        self._connect_btn.config(text="Connect")
        self._conn_indicator.config(fg="red")
        self._status.config(text="Disconnected (lost)")

    def _append_text(self, text):
        self._text.config(state=tk.NORMAL)
        self._text.insert(tk.END, text)
        # Keep display manageable — trim to last 5000 lines
        lines = int(self._text.index('end-1c').split('.')[0])
        if lines > 5000:
            self._text.delete('1.0', '{}.0'.format(lines - 5000))
        if self._autoscroll_var.get():
            self._text.see(tk.END)
        self._text.config(state=tk.DISABLED)

    def _clear_display(self):
        self._text.config(state=tk.NORMAL)
        self._text.delete('1.0', tk.END)
        self._text.config(state=tk.DISABLED)
        self._packet_count = 0
        self._bad_checksum_count = 0
        self.decoder.reset()

    def _poll_stats(self):
        if self._log_active:
            b = self.serial.bytes_logged
            if b < 1024:
                size = "{} B".format(b)
            elif b < 1048576:
                size = "{:.1f} KB".format(b / 1024)
            else:
                size = "{:.1f} MB".format(b / 1048576)
            self._stats_label.config(text="Logged: {}".format(size))
        else:
            self._stats_label.config(text="")

        # Packet decode stats
        if self._display_var.get() == self.DISPLAY_DECODED and self.decoder.loaded:
            bad = ""
            if self._bad_checksum_count > 0:
                bad = "  bad={}".format(self._bad_checksum_count)
            self._pkt_stats_label.config(text="pkts={}{}".format(self._packet_count, bad))

        self.root.after(500, self._poll_stats)

    def on_close(self):
        self.serial.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    app = SerialLoggerApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
