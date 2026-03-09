// =============================================================================
// Packet Format Configuration — Feather GFSK Bridge
// =============================================================================
// Edit this file to match your protocol. The bridge firmware uses these
// definitions in FORMAT and HYBRID modes.
//
// FORMAT mode:  Recognizes packets by header byte, validates fixed length
//               and XOR checksum, then batches verified packets into radio
//               frames.  Corrupt or unrecognized bytes are discarded.
//
// HYBRID mode:  Self-framing variable-length packets (no header table).
//               Packet boundaries are determined by inspecting the first
//               byte of each packet.  See HYBRID FRAMING RULES below.
//
// RAW mode:     Ignores this file entirely — all bytes are forwarded as-is.
// =============================================================================

#ifndef PACKET_CONFIG_H
#define PACKET_CONFIG_H

// ---------------------------------------------------------------------------
// FORMAT MODE — Packet Table
// ---------------------------------------------------------------------------
// Each entry: { header_byte, total_length }
//   header_byte   Unique first byte that identifies the packet type.
//   total_length  Fixed packet size in bytes (header + payload + checksum).
//
// Requirements:
//   - Every packet type must have a unique header byte.
//   - Length includes the header and the trailing checksum byte.
//   - Maximum single packet size: 63 bytes (SX1276 FIFO limit).
//   - Checksum is XOR of all preceding bytes; XOR of the full packet == 0x00.
//
// Add, remove, or modify entries to match your protocol.
// ---------------------------------------------------------------------------

struct PacketFormat {
  uint8_t header;
  uint8_t length;
};

#define FORMAT_COUNT 4

const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // 12 bytes: header + payload + checksum
  { 0xB5,  8 },  //  8 bytes
  { 0xC5, 10 },  // 10 bytes
  { 0xD5,  9 },  //  9 bytes
};

// ---------------------------------------------------------------------------
// HYBRID MODE — Self-Framing Rules
// ---------------------------------------------------------------------------
// Hybrid packets encode their own length in the first byte.  The bridge
// inspects bit patterns to determine packet boundaries without a format
// table.  Edit hybridPacketLen() below to match your framing scheme.
//
// Default rules (3 packet types):
//   0xFF          Sync/overflow marker — 3 bytes
//   bit 7 = 1    Group event          — 3 bytes
//   bit 7 = 0    Single event         — 2 bytes
// ---------------------------------------------------------------------------

inline uint8_t hybridPacketLen(uint8_t firstByte) {
  if (firstByte == 0xFF) return 3;   // sync / overflow
  if (firstByte & 0x80)  return 3;   // group event
  return 2;                           // single event
}

#endif // PACKET_CONFIG_H
