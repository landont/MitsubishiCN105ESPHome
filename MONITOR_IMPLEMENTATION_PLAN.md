# PEAD Health Monitor — Implementation Plan

*Companion to [`MONITOR_REQUIREMENTS.md`](./MONITOR_REQUIREMENTS.md) and
[`MONITOR_DEVELOPMENT_PLAN.md`](./MONITOR_DEVELOPMENT_PLAN.md).*
*Scope: the code-bearing work of **Stage A** (the bench build), broken into mergeable PRs.*
*Stage B (physical install) and Stage C (remote/OTA) are not code and are tracked in the dev plan.*

---

## Grounding facts (verified in-tree)

These make the plan concrete rather than aspirational:

- **Single write choke point.** Every outbound packet goes through
  `CN105Climate::writePacket()` (`hp_writings.cpp:91`). The packet **type byte is always at
  index 1**: `CONNECT={0xfc,0x5a,…}`, `SET HEADER={0xfc,0x41,…}`, `INFOHEADER={0xfc,0x42,…}`
  (`cn105_types.h:40–45`). ⇒ NFR2's allow-list is a one-line `packet[1]` check.
- **Config + registration live in `climate.py`** (776 lines). Platforms are conditionally
  registered in `to_code()` via `if CONF_xxx in config:` blocks (e.g. selects at
  `:568–590`, switches/numbers/buttons below) — gating them is mechanical.
- **SET emitters** all build via `createPacket()`→`prepareSetPacket()` then `writePacket()`
  (`hp_writings.cpp:213, 378–379, 525–542, 559–593`); INFO via `createInfoPacket()`→
  `writePacket()` (`:439–440`). CONNECT via `sendFirstConnectionPacket()` (`:28`).
- **Error decoder gap** is real: `0x04` is published as raw bytes around
  `hp_readings.cpp:447` via `error_code_sensor.h`.

---

## PR sequence (each independently mergeable, CI green)

```
PR1 monitor_only flag + writePacket guard   ← foundational, everything sits behind it
 ├─ PR2 gate/compile-out write paths         (defense in depth)
 ├─ PR3 read-only HA entity surface          (Python-only, independent)
 ├─ PR4 0x04 decoder + A2L leak alert        (pure C++ + unit tests, independent)
 └─ PR5 runtime passive/active toggle + dead-man  (depends on PR1's mode field)
PR6 OTA-resilient example config + power notes    (YAML/docs; no core code)
PR7 remote capture/log streaming             (diagnostic; can land anytime after PR1)
```

---

### PR1 — `monitor_only` flag + `writePacket()` allow-list guard  *(NFR1 backstop, NFR2)*

The foundation: a compile-time mode and the fail-safe choke point, landed **before** any
write path is touched.

- **`climate.py`**: add `CONF_MONITOR_ONLY = "monitor_only"`, `cv.Optional(..., default=False)`;
  in `to_code()` emit `cg.add(var.set_monitor_only(conf))` and, when true,
  `cg.add_define("CN105_MONITOR_ONLY")`.
- **`cn105.h`**: `bool monitor_only_{false}; void set_monitor_only(bool v){monitor_only_=v;}`
  and `uint32_t blocked_write_count_{0};`.
- **`hp_writings.cpp` `writePacket()`** — add at the top, before the UART-ready check:
  ```cpp
  if (this->monitor_only_) {
      const uint8_t type = (length > 1) ? packet[1] : 0x00;
      if (type != 0x5a && type != 0x42) {           // CONNECT and INFO only
          this->blocked_write_count_++;
          ESP_LOGE(TAG, "monitor_only: BLOCKED write type 0x%02X (count=%u)",
                   type, this->blocked_write_count_);
          return;                                    // fail safe: drop, never transmit
      }
  }
  ```
- **Tests** (`tests/unit/test_write_guard.cpp`, new): assert `0x41`/`0x08` dropped &
  counter increments; `0x5a`/`0x42` pass. Add to `tests/unit/CMakeLists.txt`.
- **Exit:** guard unit-tested; CI green; no behavior change when `monitor_only:false`.

---

### PR2 — Remove the write surface (firmware)  *(NFR1, NFR5)* — DONE

Chose **runtime early-return guards** over `#ifdef` compile-out: no signature/caller surgery,
compiles cleanly in both modes, and it layers as a distinct second tier above PR1's
already-tested byte-level `writePacket()` backstop (defense in depth).

- `monitor_only_` early-return added to: `sendWantedSettings()`,
  `sendWantedSettingsDelegate()`, `sendRemoteTemperaturePacket()`, `sendWantedRunStates()`
  (`hp_writings.cpp`); `control()` (`climateControls.cpp`) — the single entry point for all
  HA commands, which also delivers PR3's deferred **read-only climate no-op**.
- Remote-temp **watchdog + keep-alive** suppressed in monitor mode: `pingExternalTemperature()`,
  `startRemoteTempKeepAlive()` (`cn105.cpp`). NFR5.
- **Untouched** (Req §10.4): `sendFirstConnectionPacket()` (`0x5a`),
  `buildAndSendInfoPacket()`/`createInfoPacket()` (`0x42`), FSM, read path, scheduler.
- **Verified:** the monitor-only firmware **compiles to a full ESP32 image** (`esphome
  compile`, esp-idf); all 211 unit tests still pass. The three layers now stand: control()
  no-op → emitters suppressed → writePacket() drops anything but `0x5a`/`0x42`.

---

### PR3 — Read-only HA entity surface  *(NFR3, FR3, FR4)* — DONE (entity surface)

Chose the **hard schema error** over silently skipping `to_code()` blocks — a read-only
build that's handed a writable entity should fail loud, not quietly drop it.

- `climate.py`: `WRITABLE_KEYS_IN_MONITOR_MODE` list + `_validate_monitor_only()`, wired via
  `CONFIG_SCHEMA = cv.All(…, _validate_monitor_only)`. Rejects vane selects,
  `airflow_control_select`, the three `*_switch`, `functions_set_button`,
  `functions_set_code/value`, `hardware_settings`, and `remote_temperature_*` when
  `monitor_only:true`. (`functions_get_button` is allowed — GET-only, `0x42`.)
- **Verified with ESPHome 2026.5.0** (`esphome config`): valid monitor build accepted;
  `monitor_only:true` + `air_purifier_switch` rejected with a clear message;
  `monitor_only:false` + writable entity still accepted (no regression).
- Keep `sensor`/`binary_sensor`/`text_sensor`. **Deferred:** the climate entity's own
  `control()` no-op is **PR2**; the `pead_monitor_*`-named example + CI matrix variant is
  **PR6** (FR4).

---

### PR4 — `0x04` decoder + A2L refrigerant-leak alert  *(FR5)* — DONE (mechanism); table source-gated

**Key finding (verified June 2026):** there is **no public byte→error-code mapping** for
CN105 `0x04` — not in SwiCago, its wiki, or anywhere. The P/E/U byte values are
undocumented and the R454B A2L codes (`FL`/`FH`/`PL`) are brand-new with no published
encoding. So the table **cannot be sourced from docs**; it must be built from a live capture
(Stage C). We therefore shipped the *mechanism* with an **empty, source-gated table** rather
than fabricate safety-critical codes.

- `error_code_map.h` (header-only, ESPHome-free): `decode_error_mnemonic(code, sub)` and
  `is_a2l_leak(code, sub)` over `ERROR_CODE_TABLE` (**empty**); `find_error_entry()` is the
  pure lookup core so tests exercise the mechanism with a fixture.
- Wired into `hp_readings.cpp` `getErrorInfoFromResponsePacket()`: keeps raw bytes, prepends
  the mnemonic **when known** (else raw-hex fallback, unchanged behavior), and drives a new
  **`device_class: safety` binary_sensor** (`refrigerant_leak`, plumbed in `climate.py`).
  `is_a2l_leak()` returns false for every code until populated → **wired but inactive**.
- **Tests** (`tests/unit/test_error_decode.cpp`, 7 cases): fixture hit/miss, A2L flag,
  null/empty safety, production-table-empty guard, raw fallback, leak-inactive.
- **Remaining (Stage C):** capture real `0x04` frames, fill `ERROR_CODE_TABLE` from confirmed
  bytes (prioritising `FL`/`FH`/`PL`), and update the `ProductionTableEmptyUntilCaptured`
  test expectation. Until then FR5 is *mechanically* complete but *functionally* pending data.

---

### PR5 — Runtime passive/active toggle + dead-man  *(NFR6, NFR7)* — DONE

Lets Stage C validate Q2 over OTA without a re-flash.

- `monitor_guard.h`: `enum class BusMode {PASSIVE, ACTIVE}` + `outbound_packet_allowed()`
  combining bus mode with the monitor_only allow-list. PASSIVE drops **all** outbound;
  ACTIVE defers to `monitor_allows_packet()`. `writePacket()` now uses this.
- `cn105.h`/`cn105.cpp`: `bus_mode_` (default PASSIVE), `arm_active_polling()` /
  `disarm_active_polling()`, `bus_mode_str()`. Arm sets ACTIVE and (re)arms a `set_timeout`
  **dead-man** (`active_polling_deadman`, default 30 min, 0=off) that reverts to PASSIVE
  unless re-armed (NFR7). `setup()` boots PASSIVE for monitor builds, ACTIVE otherwise (so
  normal control is unchanged). Never persisted → power cycle always lands PASSIVE.
- Gated `sendFirstConnectionPacket()` and `buildAndSendRequestsInfoPackets()` to early-return
  in PASSIVE (no CONNECT/poll churn; UART still set up for RX).
- `climate.py`: `active_polling_deadman` time-period option (default `30min`).
- **HA control surface (YAML-level, not new platform code):** expose `arm_active_polling()`
  via a template `button`/`switch` in the example (PR6); avoids adding a writable platform to
  the component. (Arming is not an HVAC write — it only enables `0x42` polling.)
- **Tests** (4 new, 215 total): PASSIVE drops every type even in non-monitor builds; ACTIVE
  monitor permits only `{0x5a,0x42}`; ACTIVE non-monitor permits all; null/short safety.
- **Verified:** monitor firmware compiles to a full ESP32 image. **Deferred to Stage C:**
  N-bus-anomaly auto-disarm (only the time-based dead-man is implemented now).

---

### PR6 — OTA-resilient example config + power decision  *(NFR8, NFR9, Q3)* — YAML/docs

- `examples/pead-monitor/pead-monitor.yaml`: `monitor_only:true`, only health
  sensor/binary_sensor/text_sensor, `pead_monitor_*` prefix, UART 2400/8E1,
  `safe_mode:`, `ota:` with rollback, WiFi `ap:` fallback / captive portal. Add to CI matrix.
- Doc the A7 power-chain bench test + the final wiring decision (12 V ACC leg vs external).
- **Exit:** example compiles in CI; bench OTA-recovery + power test recorded in the dev plan.

---

### PR7 — Remote capture / log streaming  *(FR3 live-verify, §8 acceptance)*

- A diagnostic that streams the passive `FrameParser` byte log + decoded INFO codes over
  WiFi/HA so Q2 analysis and code-enumeration (Stage C) happen from the desk. Reuses the
  existing `hpPacketDebug` path behind a verbosity/diagnostic flag.
- **Exit:** captures visible remotely; supports the §8 "only `0x5a`/`0x42`" verification.

---

## Definition of done (every PR)

- Builds clean for both `monitor_only:true` and `:false` (no regression for normal users).
- Unit tests added where logic is new (PR1, PR4, PR5); `ctest` green.
- ESPHome build matrix green; `monitor_only` example variant added (PR3/PR6).
- French/English comment style matched to surrounding file; no new mojibake.

## Open items to resolve before/while coding

- **Q5 / `0x5b`**: PR1 allow-list is `{0x5a,0x42}`. If live testing (Stage C) shows the PEAD
  needs `0x5b` for Functions reads, widening the allow-list is a deliberate, documented
  change to PR1 — not a silent one (see Req §6 Q5).
- **Climate entity**: omit vs read-only no-op (PR3) — recommend read-only for setpoint/mode
  visibility (Req §9.1).
- **First mergeable now:** PR1 + PR4 are pure software with no hardware dependency and can
  start immediately; PR5–PR7 enable the remote Stage C workflow.
```
