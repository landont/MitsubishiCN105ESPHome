/// test_write_guard.cpp — Unit tests for the monitor_only outbound allow-list.
/// Deps: monitor_guard.h (pure, ESPHome-free).
///
/// Guards Requirements NFR2: in monitor_only mode the only packet types that may
/// leave the device are CONNECT (0x5a) and INFO (0x42); everything else — most
/// importantly SET (0x41) and run-states (0x08) — must be dropped.
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "monitor_guard.h"

using namespace cn105_protocol;

// Helper: build a minimal frame with the given type byte at offset 1.
static std::vector<uint8_t> frame(uint8_t type, int len = 8) {
    std::vector<uint8_t> f(static_cast<size_t>(len), 0x00);
    f[0] = 0xFC;
    if (len > 1) f[1] = type;
    return f;
}

// ════════════════════════════════════════════════════════════════
// monitor_only OFF — everything is allowed (normal control build)
// ════════════════════════════════════════════════════════════════

TEST(MonitorGuard, DisabledAllowsEverything) {
    auto set = frame(PACKET_TYPE_SET);
    auto info = frame(PACKET_TYPE_INFO);
    auto connect = frame(PACKET_TYPE_CONNECT);
    EXPECT_TRUE(monitor_allows_packet(false, set.data(), (int)set.size()));
    EXPECT_TRUE(monitor_allows_packet(false, info.data(), (int)info.size()));
    EXPECT_TRUE(monitor_allows_packet(false, connect.data(), (int)connect.size()));
    // Even a bogus type passes when not in monitor mode.
    auto bogus = frame(0x99);
    EXPECT_TRUE(monitor_allows_packet(false, bogus.data(), (int)bogus.size()));
}

// ════════════════════════════════════════════════════════════════
// monitor_only ON — allow-list = {0x5a, 0x42}
// ════════════════════════════════════════════════════════════════

TEST(MonitorGuard, AllowsConnectAndInfo) {
    auto connect = frame(PACKET_TYPE_CONNECT);
    auto info = frame(PACKET_TYPE_INFO);
    EXPECT_TRUE(monitor_allows_packet(true, connect.data(), (int)connect.size()));
    EXPECT_TRUE(monitor_allows_packet(true, info.data(), (int)info.size()));
}

TEST(MonitorGuard, BlocksSetPacket) {
    auto set = frame(PACKET_TYPE_SET);  // 0x41 — the cardinal sin
    EXPECT_FALSE(monitor_allows_packet(true, set.data(), (int)set.size()));
}

TEST(MonitorGuard, BlocksRunStatesAndOtherTypes) {
    for (uint8_t type : {uint8_t(0x08), uint8_t(0x5b), uint8_t(0x00), uint8_t(0xFF), uint8_t(0x62)}) {
        auto f = frame(type);
        EXPECT_FALSE(monitor_allows_packet(true, f.data(), (int)f.size()))
            << "type 0x" << std::hex << int(type) << " should be blocked in monitor mode";
    }
}

// ════════════════════════════════════════════════════════════════
// Defensive edge cases — never transmit when type can't be proven
// ════════════════════════════════════════════════════════════════

TEST(MonitorGuard, BlocksNullPointer) {
    EXPECT_FALSE(monitor_allows_packet(true, nullptr, 8));
}

TEST(MonitorGuard, BlocksTooShortFrame) {
    uint8_t one_byte[1] = {0xFC};
    EXPECT_FALSE(monitor_allows_packet(true, one_byte, 1));
    EXPECT_FALSE(monitor_allows_packet(true, one_byte, 0));
}

// ════════════════════════════════════════════════════════════════
// outbound_packet_allowed() — combined bus-mode + monitor_only (NFR6)
// ════════════════════════════════════════════════════════════════

TEST(BusMode, PassiveDropsEverything) {
    // PASSIVE = pure RX: nothing leaves, regardless of type or monitor_only.
    for (uint8_t type : {PACKET_TYPE_CONNECT, PACKET_TYPE_INFO, PACKET_TYPE_SET, uint8_t(0x08)}) {
        auto f = frame(type);
        EXPECT_FALSE(outbound_packet_allowed(true, BusMode::PASSIVE, f.data(), (int)f.size()));
        EXPECT_FALSE(outbound_packet_allowed(false, BusMode::PASSIVE, f.data(), (int)f.size()))
            << "PASSIVE must drop even in a non-monitor build (type 0x" << std::hex << int(type) << ")";
    }
}

TEST(BusMode, ActiveMonitorAllowsOnlyConnectAndInfo) {
    auto connect = frame(PACKET_TYPE_CONNECT);
    auto info = frame(PACKET_TYPE_INFO);
    auto set = frame(PACKET_TYPE_SET);
    EXPECT_TRUE(outbound_packet_allowed(true, BusMode::ACTIVE, connect.data(), (int)connect.size()));
    EXPECT_TRUE(outbound_packet_allowed(true, BusMode::ACTIVE, info.data(), (int)info.size()));
    EXPECT_FALSE(outbound_packet_allowed(true, BusMode::ACTIVE, set.data(), (int)set.size()));
}

TEST(BusMode, ActiveNonMonitorAllowsAll) {
    auto set = frame(PACKET_TYPE_SET);
    EXPECT_TRUE(outbound_packet_allowed(false, BusMode::ACTIVE, set.data(), (int)set.size()));
}

TEST(BusMode, PassiveBlocksNullAndShort) {
    uint8_t one[1] = {0xFC};
    EXPECT_FALSE(outbound_packet_allowed(true, BusMode::PASSIVE, nullptr, 8));
    EXPECT_FALSE(outbound_packet_allowed(true, BusMode::ACTIVE, nullptr, 8));
    EXPECT_FALSE(outbound_packet_allowed(true, BusMode::ACTIVE, one, 1));
}
