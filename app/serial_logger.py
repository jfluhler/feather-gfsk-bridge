#!/usr/bin/env python3
"""
Standalone Serial Logger — DAQ-agnostic serial data capture to file.

Logs raw serial data to timestamped files. Works with any serial device
(LoRa RX, UART bridges, debug consoles, etc.).
"""

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


class SerialLoggerApp:
    """Standalone serial logger GUI."""

    BAUD_RATES = ["9600", "19200", "38400", "57600", "115200",
                  "230400", "460800", "921600", "1000000", "5000000"]
    DISPLAY_HEX = "Hex"
    DISPLAY_ASCII = "ASCII"
    DISPLAY_BOTH = "Both"

    def __init__(self, root):
        self.root = root
        self.root.title("Serial Logger")
        self.root.geometry("750x550")
        self.root.minsize(600, 400)

        self.serial = SerialLogger()
        self.serial.on_data = self._on_data
        self.serial.on_disconnect = self._on_disconnect

        self._port_map = {}
        self._log_active = False
        self._log_path = None

        self._build_ui()
        self._refresh_ports()
        self._update_filename_preview()

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
        for mode in [self.DISPLAY_HEX, self.DISPLAY_ASCII, self.DISPLAY_BOTH]:
            ttk.Radiobutton(disp_toolbar, text=mode, variable=self._display_var,
                            value=mode).pack(side=tk.LEFT, padx=2)

        self._autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(disp_toolbar, text="Auto-scroll", variable=self._autoscroll_var).pack(side=tk.LEFT, padx=10)
        ttk.Button(disp_toolbar, text="Clear", command=self._clear_display).pack(side=tk.RIGHT, padx=2)

        self._text = tk.Text(disp_frame, wrap=tk.WORD, font=("Consolas", 9), height=15,
                             state=tk.DISABLED, bg="#1e1e1e", fg="#d4d4d4",
                             insertbackground="#d4d4d4", selectbackground="#264f78")
        scroll = ttk.Scrollbar(disp_frame, orient=tk.VERTICAL, command=self._text.yview)
        self._text.config(yscrollcommand=scroll.set)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self._text.pack(fill=tk.BOTH, expand=True)

        # Status bar
        self._status = ttk.Label(main, text="Disconnected", relief=tk.SUNKEN, anchor=tk.W)
        self._status.pack(fill=tk.X, pady=(5, 0))

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
                # We'll handle hex writing in _on_data
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
        if self._log_active and hasattr(self, '_hex_log_mode') and self._hex_log_mode:
            if self.serial._log_file:
                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                hex_str = data.hex(' ')
                self.serial._log_file.write("[{}] {}\n".format(ts, hex_str))
                self.serial._log_file.flush()
                self.serial.bytes_logged += len(data)

        # Display in text widget
        mode = self._display_var.get()
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
