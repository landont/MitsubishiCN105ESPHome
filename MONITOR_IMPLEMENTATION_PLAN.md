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

### PR2 — Remove the write surface (firmware)  *(NFR1, NFR5)*

Make SET emission *unreachable*, with PR1 as the backstop if anything slips.

- Wrap in `#ifndef CN105_MONITOR_ONLY` (or early-return on `monitor_only_`):
  `sendWantedSettings()/sendWantedSettingsDelegate()/createPacket()`,
  `sendRemoteTemperature()/sendRemoteTemperaturePacket()`, `sendWantedRunStates()`
  (`hp_writings.cpp`); `control()/controlDelegate()/controlMode/Temperature/Fan/Swing()`
  (`climateControls.cpp`).
- Disable remote-temp **watchdog + keepalive** timers in monitor mode (`cn105.cpp`
  `pingExternalTemperature`, `startRemoteTempKeepAlive`). NFR5.
- **Leave untouched** (Req §10.4): `sendFirstConnectionPacket()` (`0x5a`),
  `buildAndSendInfoPacket()`/`createInfoPacket()` (`0x42`), FSM, read path, scheduler.
- **Exit:** grep shows no reachable `0x41` builder in monitor mode; PR1 guard re-tested with
  a deliberately-injected write still catches it.

---

### PR3 — Read-only HA entity surface  *(NFR3, FR3, FR4)*

Python-only; no writable platform may register.

- In `to_code()`, skip these `if CONF_… in config:` blocks when `monitor_only`: vane selects
  (`:568–590`), `airflow_control_select`, the three `*_switch`, `functions_set_button`,
  `functions_set_code/value`, `hardware_settings`, `remote_temperature_*`. Prefer a hard
  schema error if a writable key is set alongside `monitor_only:true` (fail loud at compile).
- Keep `sensor`/`binary_sensor`/`text_sensor`. Climate entity: omit or read-only no-op
  (its `control()` already neutered by PR2).
- Enforce distinct `pead_monitor_*` naming guidance in the example (PR6). FR4.
- **Exit:** a `monitor_only` config compiles; ESPHome build matrix variant added.

---

### PR4 — `0x04` decoder + A2L refrigerant-leak alert  *(FR5)* — the only net-new logic

- New `error_code_map.h` (header-only, ESPHome-free so it unit-tests like `frame_parser.h`):
  `const char* decode_error_mnemonic(uint8_t raw)` covering the P/E/U/F tables in Req §6 Q4,
  plus `bool is_a2l_leak(uint8_t raw)` for `FL`/`FH`/`PL`.
- Wire into `error_code_sensor.h` / `hp_readings.cpp:447`: keep raw byte, add mnemonic text,
  and publish a **distinct high-severity `binary_sensor`** for the A2L leak (new config key
  `refrigerant_leak`).
- **Tests** (`tests/unit/test_error_decode.cpp`): known codes→mnemonics; `FL/FH/PL`→leak
  asserted; unknown→graceful raw passthrough. *(A2L can't be tested on hardware — tests are
  the proof.)*
- **Exit:** decoder unit-tested; leak binary_sensor in the example.

---

### PR5 — Runtime passive/active toggle + dead-man  *(NFR6, NFR7)*

Lets Stage C validate Q2 over OTA without a re-flash. Depends on PR1.

- **`cn105.h`**: `enum class BusMode { PASSIVE, ACTIVE }; BusMode bus_mode_{BusMode::PASSIVE};`
  (passive on boot, always). Extend PR1 guard: in `PASSIVE`, drop **all** outbound
  (including `0x5a`/`0x42`) — pure RX.
- Gate active polling: `buildAndSendRequestsInfoPackets()` / `sendFirstConnectionPacket()`
  early-return when `bus_mode_ == PASSIVE`.
- **HA control surface** (read-only-safe): a `button`/`switch` `arm_active_polling` that sets
  `ACTIVE`; a dead-man `set_interval` that reverts to `PASSIVE` after a re-arm window lapses
  or after N bus anomalies (NFR7). Optional `text_sensor` exposing current `BusMode` +
  `DriverState`.
- **Tests**: PASSIVE drops everything; ACTIVE permits only `{0x5a,0x42}`; dead-man reverts.
- **Exit:** mode toggles at runtime; power cycle → PASSIVE.

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
