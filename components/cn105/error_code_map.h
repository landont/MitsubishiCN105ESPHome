/// error_code_map.h — CN105 0x04 abnormal-state decoding.
/// Deps: none (pure, ESPHome-free — unit-testable like frame_parser.h).
///
/// ─────────────────────────────────────────────────────────────────────────
/// SOURCE STATUS (verified June 2026): there is NO public authoritative
/// byte→error-code mapping for the CN105 0x04 packet.
///   • SwiCago/HeatPump and its "Mitsubishi protocol" wiki document only that
///     0x04 is an error-info packet and that 0x80 means "normal operation".
///   • The per-code byte values for the P/E/U display codes are undocumented.
///   • The R454B A2L codes FL/FH/PL are new in 2024 and have no published
///     CN105 encoding at all.
///
/// Therefore ERROR_CODE_TABLE is intentionally EMPTY: until real (code, sub)
/// byte pairs are confirmed by a live capture on the target unit (dev plan
/// Stage C), decode_error_mnemonic() returns nullptr and the caller falls back
/// to the existing raw-hex output. Populating it is then a one-line-per-code
/// data edit — the mechanism and tests below already work.
///
/// DO NOT add speculative entries. A wrong mnemonic on a refrigerant-leak
/// (A2L) alert is worse than none — it manufactures false confidence about a
/// flammable-refrigerant safety signal (Requirements FR5).
/// ─────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

namespace cn105_protocol {

/// One confirmed mapping from a raw CN105 0x04 (code, sub) pair to a Mitsubishi
/// display mnemonic. `a2l_leak` flags the R454B refrigerant-leak codes
/// (FL/FH/PL) that must be surfaced as a high-severity safety signal.
struct ErrorCodeEntry {
    uint8_t code;          // data[4] & 0x7F
    uint8_t sub;           // data[5]
    const char* mnemonic;  // e.g. "P4", "E6", "FL"
    bool a2l_leak;         // true only for confirmed FL/FH/PL
};

/// Pure lookup over an arbitrary table — lets tests exercise the mechanism with
/// a fixture without depending on the (currently empty) production table.
inline const ErrorCodeEntry* find_error_entry(const ErrorCodeEntry* table, int len,
                                              uint8_t code, uint8_t sub) {
    if (table == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < len; i++) {
        if (table[i].code == code && table[i].sub == sub) {
            return &table[i];
        }
    }
    return nullptr;
}

/// Production table — EMPTY by design (see SOURCE STATUS above). Populate from
/// verified captures only, e.g.  { 0x.., 0x.., "P4", false },
static constexpr ErrorCodeEntry ERROR_CODE_TABLE[] = {
    {0, 0, nullptr, false},  // placeholder so the array is well-formed; len computed below
};
/// Real entry count (excludes the placeholder). Becomes >0 once populated.
static constexpr int ERROR_CODE_TABLE_LEN = 0;

/// Translate a raw (code, sub) pair to a display mnemonic, or nullptr if the
/// pair is not in the confirmed table (caller should fall back to raw hex).
inline const char* decode_error_mnemonic(uint8_t code, uint8_t sub) {
    const ErrorCodeEntry* e = find_error_entry(ERROR_CODE_TABLE, ERROR_CODE_TABLE_LEN, code, sub);
    return e ? e->mnemonic : nullptr;
}

/// True only for a confirmed A2L refrigerant-leak code. Returns false for every
/// pair until the table is populated — wired but inactive (Requirements FR5).
inline bool is_a2l_leak(uint8_t code, uint8_t sub) {
    const ErrorCodeEntry* e = find_error_entry(ERROR_CODE_TABLE, ERROR_CODE_TABLE_LEN, code, sub);
    return e ? e->a2l_leak : false;
}

}  // namespace cn105_protocol
