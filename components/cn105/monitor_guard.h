/// monitor_guard.h — Outbound-packet allow-list for monitor_only mode.
/// Deps: none (pure, ESPHome-free — unit-testable like frame_parser.h).
///
/// In monitor_only mode the firmware must never transmit anything that could
/// change the heat pump's state. The CN105 packet TYPE byte is always at index 1
/// (0xfc <TYPE> ...). Only CONNECT (0x5a) and INFO/GET (0x42) are permitted; any
/// other type — most importantly SET (0x41) and run-states (0x08) — is dropped.
///
/// This is the single, caller-agnostic choke point (Requirements NFR2): it holds
/// even if a future change reintroduces a write path upstream of writePacket().
#pragma once

#include <cstdint>

namespace cn105_protocol {

/// Runtime bus participation mode (Requirements NFR6). Independent of the
/// compile-time monitor_only flag: PASSIVE is RX-only (transmits nothing at
/// all), ACTIVE may transmit per the monitor_only allow-list. A monitor build
/// boots PASSIVE on every power cycle and is *armed* to ACTIVE at runtime.
enum class BusMode : uint8_t {
    PASSIVE = 0,  // pure listen: drop every outbound packet
    ACTIVE = 1,   // may transmit (subject to monitor_only allow-list)
};

/// CN105 packet type bytes (offset 1 in every frame).
static constexpr uint8_t PACKET_TYPE_CONNECT = 0x5a;  // handshake
static constexpr uint8_t PACKET_TYPE_SET     = 0x41;  // write — forbidden in monitor mode
static constexpr uint8_t PACKET_TYPE_INFO    = 0x42;  // GET/poll

/// Decide whether an outbound packet may be transmitted.
///
/// @param monitor_only  When false, everything is allowed (normal control build).
/// @param packet        Raw frame buffer (packet[1] is the type byte).
/// @param length        Frame length in bytes; a too-short frame is never allowed
///                      in monitor mode (cannot prove its type).
/// @return true if the packet may be sent.
///
/// NOTE: widening this allow-list (e.g. to permit the 0x5b installer handshake for
/// Functions reads — Requirements §6 Q5) is a deliberate, documented change here,
/// never a silent one.
inline bool monitor_allows_packet(bool monitor_only, const uint8_t* packet, int length) {
    if (!monitor_only) {
        return true;
    }
    if (packet == nullptr || length < 2) {
        return false;
    }
    const uint8_t type = packet[1];
    return (type == PACKET_TYPE_CONNECT || type == PACKET_TYPE_INFO);
}

/// Full outbound decision combining bus mode (NFR6) with the monitor_only
/// allow-list. PASSIVE transmits nothing whatsoever; ACTIVE defers to
/// monitor_allows_packet(). This is the single source of truth used by
/// writePacket().
inline bool outbound_packet_allowed(bool monitor_only, BusMode mode,
                                    const uint8_t* packet, int length) {
    if (mode == BusMode::PASSIVE) {
        return false;  // RX-only: never transmit, not even CONNECT/INFO
    }
    return monitor_allows_packet(monitor_only, packet, length);
}

}  // namespace cn105_protocol
