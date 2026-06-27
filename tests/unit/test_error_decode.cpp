/// test_error_decode.cpp — Unit tests for the CN105 0x04 error-code decoder.
/// Deps: error_code_map.h (pure, ESPHome-free).
///
/// These prove the *mechanism* (FR5): table lookup, raw-hex fallback, and the
/// A2L leak flag. The production ERROR_CODE_TABLE is intentionally empty until
/// real byte codes are confirmed by live capture (dev plan Stage C), so the
/// mechanism is exercised against a local fixture table via find_error_entry().
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include "error_code_map.h"

using namespace cn105_protocol;

// Fixture table standing in for a future capture-confirmed mapping. The byte
// values here are illustrative ONLY — they are not claimed to be real codes.
static constexpr ErrorCodeEntry kFixture[] = {
    {0x04, 0x00, "P4", false},  // float-switch (illustrative)
    {0x12, 0x00, "E6", false},  // indoor/outdoor comms (illustrative)
    {0x40, 0x01, "FL", true},   // A2L refrigerant leak (illustrative)
};
static constexpr int kFixtureLen = sizeof(kFixture) / sizeof(kFixture[0]);

// ════════════════════════════════════════════════════════════════
// Mechanism: find_error_entry() over an arbitrary table
// ════════════════════════════════════════════════════════════════

TEST(ErrorDecode, FixtureHitReturnsEntry) {
    const ErrorCodeEntry* e = find_error_entry(kFixture, kFixtureLen, 0x04, 0x00);
    ASSERT_NE(e, nullptr);
    EXPECT_STREQ(e->mnemonic, "P4");
    EXPECT_FALSE(e->a2l_leak);
}

TEST(ErrorDecode, FixtureMatchRequiresBothCodeAndSub) {
    // Right code, wrong sub → miss.
    EXPECT_EQ(find_error_entry(kFixture, kFixtureLen, 0x04, 0x99), nullptr);
    // Right sub, wrong code → miss.
    EXPECT_EQ(find_error_entry(kFixture, kFixtureLen, 0x99, 0x00), nullptr);
}

TEST(ErrorDecode, FixtureA2lEntryFlagged) {
    const ErrorCodeEntry* e = find_error_entry(kFixture, kFixtureLen, 0x40, 0x01);
    ASSERT_NE(e, nullptr);
    EXPECT_STREQ(e->mnemonic, "FL");
    EXPECT_TRUE(e->a2l_leak);
}

TEST(ErrorDecode, NullOrEmptyTableIsSafe) {
    EXPECT_EQ(find_error_entry(nullptr, 0, 0x04, 0x00), nullptr);
    EXPECT_EQ(find_error_entry(kFixture, 0, 0x04, 0x00), nullptr);
}

// ════════════════════════════════════════════════════════════════
// Production table: empty by design → raw-hex fallback, leak inactive.
// (When Stage C populates ERROR_CODE_TABLE, ASSERTs below change with it.)
// ════════════════════════════════════════════════════════════════

TEST(ErrorDecode, ProductionTableEmptyUntilCaptured) {
    EXPECT_EQ(ERROR_CODE_TABLE_LEN, 0)
        << "If you populated the table from a capture, update this expectation.";
}

TEST(ErrorDecode, UnknownCodeFallsBackToRaw) {
    // No confirmed entries yet → every lookup misses → caller uses raw hex.
    EXPECT_EQ(decode_error_mnemonic(0x04, 0x00), nullptr);
    EXPECT_EQ(decode_error_mnemonic(0x12, 0x00), nullptr);
}

TEST(ErrorDecode, A2lLeakInactiveUntilPopulated) {
    // Wired but inactive: the leak flag must never fire on a guessed code.
    EXPECT_FALSE(is_a2l_leak(0x40, 0x01));
    EXPECT_FALSE(is_a2l_leak(0x00, 0x00));
}
