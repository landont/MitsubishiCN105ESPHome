# PEAD Health Monitor — Requirements

*Status: draft / requirements only — no code written yet.*
*Target: a read-only (query-only) ESPHome build derived from this codebase that reports
health and status telemetry from a Mitsubishi PEAD indoor unit, alongside an existing
Airzone controller.*

---

## 1. Goal & Scope

Add a **monitor-only** variant of this firmware that connects to the CN105 port of a
Mitsubishi **PEAD-A24AA7** air handler (outdoor unit **PUZ-AH24NL**) and publishes its
status/health to Home Assistant.

**In scope — queries (reads):**
- CN105 `CONNECT` handshake (`0x5a`) to establish communication.
- Cyclic `INFO`/GET polling (`0x42`) of status codes.
- Decoding `0x62` data responses and publishing them as sensors.

**Out of scope — control (writes):** the device must **never** change the unit's
operating state. Specifically, no setpoint, mode, fan speed, vane, power on/off, or any
other write. Control of the HVAC is owned exclusively by the Airzone system.

> **Clarification from the system owner:** "No control" means no control over
> temperature, fan speed, system state, etc. It does **not** mean passive sniffing —
> the monitor is expected to actively query the PEAD for status.

---

## 2. Physical / Bus Context

| Element | Detail |
|---|---|
| Indoor unit (target) | Mitsubishi **PEAD-A24AA7**, CN105 5-pin connector |
| Outdoor unit | PUZ-AH24NL (R454B) |
| Existing master on the bus | Airzone **AZX8GTCMEL** MEL gateway (controls the PEAD) |
| Tap point | **AZX6ACCSPLMEL** CN105 splitter — `IU → CN105`, `AZ → AZX8GTCMEL`, `ACC → this monitor` |
| Monitor MCU | ESP32 (ESP32-S3 recommended per project README) |
| Link parameters | **2400 baud, 8E1** (`SERIAL_8E1`), CN105 5-pin (12V / GND / 5V / TX / RX) |

**Multi-master assumption (owner-provided):** the AZX8GTCMEL tolerates multiple masters
on the MEL/CN105 bus, so this monitor may act as an additional active master that issues
its own `CONNECT` + `INFO` polls. This is the load-bearing assumption for the whole
design — see Risks (§7).

> This replaces the earlier "must be a passive listen-only sniffer" assumption. Because
> the gateway accepts multiple masters, the monitor can use this codebase's normal
> request/response flow with the **write paths removed**, rather than a new passive
> frame-only parser.

---

## 3. Functional Requirements

- **FR1 — Connect.** Perform the CN105 `CONNECT` handshake and reach the `CONNECTED`
  state, reconnecting automatically on timeout (existing FSM in `cn105.h` /
  `componentEntries.cpp` is reused unchanged).
- **FR2 — Poll status.** Cyclically issue INFO requests for at least:
  | Code | Meaning | Existing handler |
  |---|---|---|
  | `0x02` | Settings (power, mode, setpoint, fan, vane) — *reported read-only* | `hp_readings.cpp` |
  | `0x03` | Room temperature | `getRoomTemperatureFromResponsePacket()` |
  | `0x06` | Operating status + compressor frequency | `getOperatingAndCompressorFreqFromResponsePacket()` |
  | `0x09` | Standby / power / input power / kWh | `hp_readings.cpp` |
  | `0x04` | Error / abnormal state code | `error_code_sensor.h` |
  | `0x20` / `0x22` | Functions | `functions_sensor.h` |
- **FR3 — Publish health sensors.** Expose the telemetry already supported by this
  codebase, prioritising health/diagnostics:
  - Room temperature, outside air temperature
  - Compressor frequency (energy/health proxy)
  - Operating state / stage / sub-mode
  - Input power, kWh, runtime hours
  - Error/abnormal-state code
  - Connection uptime / link health
  - Reported (read-only) setpoint, mode, fan, vane for visibility
- **FR4 — Distinct HA identity.** Use a device name/entity prefix distinct from the
  Airzone integration (e.g. `pead_monitor_*`) so entities do not collide or get confused
  with Airzone-provided ones.
- **FR5 — Decode error mnemonics (incl. A2L leak codes).** Map the raw `0x04` abnormal-state
  byte to the Mitsubishi check codes (P/E/U/F series per §6 Q4) instead of publishing raw
  hex. **Must** decode the R454B A2L codes `FL` (refrigerant leakage), `FH` (refrigerant
  sensor error) and `PL` (refrigerant circuit abnormal), and surface a leak as a distinct,
  high-severity signal (binary_sensor / alert), since R454B is mildly flammable. The current
  decoder (`error_code_sensor.h` → `hp_readings.cpp:447`) only emits raw bytes and must be
  extended.

---

## 4. Non-Functional / Safety Requirements (the "no control" guarantee)

These are the requirements that make this a *monitor*. The firmware must make it
structurally impossible to emit a write.

- **NFR1 — No SET packets.** The firmware must never transmit a CN105 `0x41` (SET)
  packet. All of the following write paths must be removed or compiled out:
  - `sendWantedSettings()` / `sendWantedSettingsDelegate()` / `createPacket()`
    — setpoint/mode/fan/vane/power (`hp_writings.cpp`)
  - `sendRemoteTemperature()` / `sendRemoteTemperaturePacket()` — remote room-temp
    injection (this writes to the unit and influences its thermostat; treat as control)
  - `sendWantedRunStates()` — air purifier / night mode / circulator
  - `control()` / `controlDelegate()` / `controlMode/Temperature/Fan/Swing()`
    (`climateControls.cpp`)
  - `*Select::control()`, switch/number/button writes, hardware-setting writes
- **NFR2 — Only `0x5a` and `0x42` may be written.** The sole permitted outbound packet
  types are `CONNECT` (`0x5a`) and `INFO`/GET (`0x42`). Recommend a single guarded
  choke point (e.g. in `writePacket()`) that asserts/drops any non-`{0x5a,0x42}` packet,
  as defense-in-depth even after the write paths are removed.
- **NFR3 — No controllable entities exposed to HA.** Either omit the writable
  `climate`/`select`/`switch`/`number`/`button` platforms entirely, or expose a climate
  entity that is read-only (no-op `control()`), so a user/automation cannot send a
  command that turns into a bus write.
- **NFR4 — Non-interference.** Monitor polling must not degrade Airzone↔PEAD control.
  Polling cadence must be conservative/configurable; the device must yield/back off and
  never hold the bus.
- **NFR5 — Read-only at rest.** No remote-temperature watchdog, keepalive, or any timer
  that could emit a write (`set_remote_temp_timeout`, keepalive interval, etc. disabled).

### Single-install / OTA constraints (NFR6–NFR9)

The device installs in a hard-to-reach location (attic) and is physically accessed **once**.
All subsequent validation and iteration — including the Q2 multi-master experiment (§7) —
happen remotely over OTA/WiFi. These requirements make that possible and safe. Note the
split: `monitor_only` (no `0x41`, NFR1) stays a **compile-time** guarantee; only polling
aggressiveness is **runtime**-controllable.

- **NFR6 — Runtime safety-mode toggle.** Passive RX-only vs. active polling
  (`0x5a`/`0x42`) is a **runtime** flag exposed to HA, **defaulting to passive on every
  boot**. Active polling is *armed*, never the cold-start default. The installed image must
  contain both capabilities so the mode can change without a re-flash. `monitor_only`
  (no `0x41`) remains compile-time and immutable.
- **NFR7 — Active-mode dead-man.** When active polling is armed, it auto-reverts to passive
  unless periodically re-armed (or after N observed bus anomalies). A power cycle always
  lands in the safe (passive) state.
- **NFR8 — OTA must survive bad flashes.** ESPHome `safe_mode` + OTA rollback + WiFi **AP
  fallback** are configured so a bad image or a dropped network self-recovers without
  physical access. Bus-polling state must never be able to take down WiFi/OTA — they are
  independent, so an aggressive-polling mistake is always remotely reversible.
- **NFR9 — Reachability margin.** The install location must have verified WiFi RSSI
  headroom; if marginal, it is remediated during the one physical visit (it is the only
  failure mode not fixable remotely).

---

## 5. Implementation Approach (proposed, not yet built)

Reuse this repo as the base. Two viable strategies — pick during design:

1. **Compile-time monitor mode (recommended).** Add a `monitor_only: true` config flag in
   `climate.py` that (a) skips registration of all writable platforms, (b) compiles out /
   stubs the SET paths, and (c) installs the `writePacket()` guard (NFR2). Keeps a single
   codebase and lets upstream fixes flow in.
2. **Forked minimal build.** Strip `hp_writings.cpp` to connect + INFO only and drop the
   control/select/switch/number/button sources. Smaller surface, but diverges from
   upstream.

Either way, the read path (`hp_readings.cpp`, `frame_parser.h`, scheduler, FSM, sensors)
is reused largely as-is.

**Reference YAML shape** (to be finalised):
```yaml
uart:
  id: HP_UART
  baud_rate: 2400
  tx_pin: GPIO43        # board-specific
  rx_pin: GPIO44
  # 8E1 is configured by the component

# climate / select / switch / number / button platforms intentionally omitted
# only sensor + binary_sensor + text_sensor for health telemetry
```

---

## 6. Manual Verification Findings (Q1 & Q4)

*Verified 2026-06-23 against the PDFs in `../Airzone/manuals`.*

### Q1 — AZX6ACCSPLMEL splitter: **RESOLVED — supports an active master on the ACC port.**

Source: `Airzone_AZX6ACCSPLMEL_Spec_Sheet.pdf`.

- The device is described as a **multiplier** that *"manages communications between
  accessories that connect to the CN105 port and allows more than one device to be
  connected."* It is an active multiplexer, **not** a dumb Y-cable — it is purpose-built
  to host multiple devices on one CN105 port.
- Three ports: **IU** → indoor unit CN105, **AZ** → Airzone gateway (AZX8GTCMEL), **ACC**
  → *"your Mitsubishi Electric device / CN105 Accessory"*. **The ACC port is our tap
  point** and is explicitly intended for an additional CN105 accessory.
- Cabling per port = **5 wires: 2 × 0.25 mm² communication wires (TX + RX, bidirectional)
  + 3 × 0.25 mm² power wires.** The self-diagnosis LED table shows **both** *data
  transmission* (red) **and** *data reception* (green) on each of IU/AZ/ACC — confirming
  bidirectional comms are routed to the ACC port. ⇒ **An active master can be attached to
  ACC.** This also reinforces the multi-master assumption (§2): Airzone designed this to
  run AZ + ACC simultaneously against one IU.
- Electrical: **12 Vdc, 160 mA max** (splitter's own draw), **10 m** max run, 2.5 m cable
  supplied. Only **one** spare ACC port exists (3 total) — this monitor consumes it. This
  is exactly the *"MEL-to-MQTT bridge"* open item already noted in the Airzone inventory.

> Net: §2's wiring assumption is validated by the spec. The remaining risk is purely
> *protocol-level* arbitration (Q2), not whether the ACC port carries TX.

### Q4 — Which data / codes the PEAD exposes: **PARTIALLY RESOLVED.**

Source: **`Mitsubishi_PEAD-AA24NL_Service_Manual_R454B.pdf`** — the correct **2024 R454B**
service manual (doc **HWE24030**, Series PEAD ceiling-concealed, covering
PEAD-AA09NL…AA24NL…AA42NL), wiring §7, refrigerant §8, self-check §12. This supersedes
the earlier 2016 R410A `PEAD-A24AA7` manual (HWE16080) used in the first pass.

> **Model-name note:** the installed R454B indoor unit is **PEAD-AA24NL** (not
> `PEAD-A24AA7`, which is the older R410A model). The Airzone inventory and the on-file
> install-manual filename still say "PEAD-A24AA7" — worth correcting to PEAD-AA24NL.

**Confirmed present (high confidence the monitor can surface these):**
- **CN105 connector** is on the indoor controller board (wiring §7 connector list) — the
  physical tap point is real. *(Identical to the R410A revision.)*
- Indoor thermistors **unchanged from R410A**: **TH1 = room/intake-air temp** (→ CN105 room
  temp, code `0x03`), **TH2 = liquid-pipe temp**, **TH5 = condenser/evaporator coil temp**.
  Plus drain float switch, drain pump, fan motor status.
- **Abnormal/error state (code `0x04`)** — self-check catalog confirmed for this R454B unit.
  The R454B manual **adds A2L refrigerant-leak codes** absent from the R410A version, which
  matters because R454B is mildly flammable — leak detection is a first-class health signal:
  - *Indoor (pattern A):* `P1` intake/room thermistor (TH1), `P2`/`P9` pipe/liquid
    thermistor (TH2), `P4` **float-switch connector open**, `P5` drain pump, `P6`
    freeze/overheat safeguard, `P8` pipe-temp, `E6`/`E7`/`EE` indoor↔outdoor comms,
    `E4`/`E5` wired-RC↔indoor comms, `E0`/`E3` RC transmission, `E1`/`E2` RC board,
    `Fb` indoor control board, **`FL` refrigerant leakage (NEW, A2L)**,
    **`FH` refrigerant sensor error (NEW, A2L)**, **`PL` refrigerant circuit abnormal (NEW)**.
  - *Outdoor (pattern B):* `E9`, `UP`/`UF`/`U6` compressor overcurrent, `U3`/`U4` outdoor
    thermistors, `U2` high discharge temp / low refrigerant, `U1`/`Ud` high pressure /
    overheat, `U5` heat-sink temp, `U8` outdoor fan, `U7` superheat / low discharge,
    `U9`/`UH` over-/under-voltage / current-sensor, **`FL` refrigerant leak/sensor error
    from another room (NEW, A2L)**.
  - ⇒ The `0x04` error sensor is genuinely valuable here (drain-pump, compressor, **and
    A2L refrigerant-leak** visibility) — prioritise it in FR3.

> **Decoder gap (new requirement):** the codebase publishes `0x04` as **raw bytes**
> (`error_code_sensor.h` → `hp_readings.cpp:447`, e.g. `"Error 0x12 sub 0x00"`); it does
> **not** translate to the Mitsubishi mnemonics above. The raw byte is still captured, but
> for usable alerting add a raw-`0x04`→mnemonic map, prioritising the A2L codes
> **`FL`/`FH`/`PL`**. Tracked as **FR5** below.
>
> **⚠ No public byte→code mapping exists (verified June 2026).** SwiCago/HeatPump and its
> "Mitsubishi protocol" wiki document only that `0x04` is an error-info packet and that
> `0x80` = normal operation; the per-code byte values (P/E/U series) are **undocumented**,
> and the 2024 R454B A2L codes (`FL`/`FH`/`PL`) have **no published CN105 encoding at all**.
> ⇒ The byte→mnemonic table **cannot be sourced from documentation** and must be built from
> a **live capture** on the target unit (dev plan Stage C). FR5's *mechanism* (decoder +
> A2L binary_sensor + tests) is implemented now with an **empty, source-gated table**
> (`error_code_map.h`) that falls back to raw hex; no speculative codes ship, because a
> wrong mnemonic on a flammable-refrigerant leak alert is worse than none.

**Uncertain — must be probed live (do not assume):**
- **Compressor frequency** (`0x06`), **input power / kWh** (`0x09`), **outside-air temp**
  are **outdoor-unit** quantities; the indoor board does not measure them locally. Whether
  they are relayed onto CN105 is firmware-dependent, and **P-series ducted units (PEAD)
  commonly expose less than M-series wall units.** Treat these as best-effort: keep them
  registered, let the scheduler's failure-counter auto-disable codes the unit NAKs/ignores
  (existing `InfoRequest.maxFailures` mechanism), and only commit them to FR3 after a live
  capture confirms non-empty responses.

**Action:** after first connect, capture a full cycle and enumerate which INFO codes
return populated `0x62` data; prune the scheduler / sensor list accordingly. The firmware
already degrades gracefully (auto-disable on repeated failure), so an unsupported code is
non-fatal — it just shouldn't be advertised as a guaranteed sensor.

### Still open

- **Q2 — Multi-master confirmation (now the top open item).** §2 wiring is confirmed; the
  remaining unknown is whether simultaneous Airzone + monitor polling causes the PEAD to
  drop/NAK/mis-serve either master at the protocol level. Validate empirically.
- **Q3 — Power source.** ACC leg carries 12 Vdc on its 3 power wires; original CN105
  builds typically buck this for the ESP. Confirm headroom for ESP32-S3 + WiFi current
  spikes, or power the ESP externally (safer). Not blocked by the manuals.
- **Q5 — Handshake, and whether `0x5b` is permitted for Functions reads.** `0x5a`
  (standard) vs `0x5b` (installer) — standard is the safer default for a read-only observer.
  **Open decision:** the README's "Hardware Settings (Function Settings)" section notes that
  some ducted units (e.g. SEZ) return **all-zeros** for the Functions codes (`0x20`/`0x22`)
  under the standard handshake and require `installer_mode: true` (the `0x5b` extended
  handshake) to unlock real values. This collides with NFR2 (allow-list is currently
  `{0x5a, 0x42}` only) and §10.3 (`installer_mode` kept only for the read-only handshake
  choice). If the PEAD returns zeros for `0x20`/`0x22`, decide explicitly:
  (a) accept that Functions data is unavailable in monitor mode, or (b) widen the NFR2
  allow-list to permit `0x5b`. `0x5b` is a handshake, not a `0x41` SET — but it requests
  installer/service privileges, so it must be an explicit, documented allowance, never a
  silent one. Validate live (the all-zeros case is auto-detected and disables polling).

---

## 7. Risks

- **Bus contention (highest).** If the multi-master assumption (§2) is wrong in practice,
  the monitor's `CONNECT`/`INFO` writes could collide with Airzone traffic and disrupt
  HVAC control. Mitigation: conservative polling, ability to fall back to a fully passive
  RX-only mode (decode whatever the Airzone polling already elicits, send nothing).
- **Entity confusion in HA.** Monitor entities mistaken for control entities. Mitigation:
  NFR3 + distinct naming (FR4).
- **Accidental write regression.** A future merge re-introduces a write path. Mitigation:
  the `writePacket()` allow-list guard (NFR2) fails safe regardless of caller.

---

## 8. Acceptance Criteria

- [ ] Device reaches `CONNECTED` and publishes the FR3 sensors to HA.
- [ ] A full bus capture over a representative period shows **only** `0x5a` and `0x42`
      packets originating from the monitor — **zero** `0x41` packets.
- [ ] Airzone retains full control with the monitor attached; no observed loss of Airzone
      command responsiveness or zone behavior.
- [ ] No writable HVAC entity is exposed in Home Assistant by the monitor device.
- [ ] Monitor survives disconnect/reconnect cycles without ever emitting a write.

---

## 9. Monitored Data Points (Kept)

Everything the monitor build **reads and publishes**. All are populated from `0x62`
data responses elicited by `0x42` INFO polls (read-only). "Live-verify" = the unit may not
support it on a PEAD ducted unit (§6 Q4); keep registered, let `maxFailures` auto-disable.

### 9.1 Climate state — reported read-only
Sourced from settings (`0x02`), room temp (`0x03`), status (`0x06`). Exposed as sensors /
a read-only climate entity, **never** as setpoints the user can change (see §10).

| Data point | INFO code | Notes |
|---|---|---|
| Current room temperature | `0x03` | TH1 intake-air thermistor (confirmed §6 Q4) |
| Power state (on/off) | `0x02` | reported only |
| Operating mode (heat/cool/dry/fan/auto) | `0x02` | reported only |
| Target setpoint (Airzone's commanded value) | `0x02` | reported only — visibility into what Airzone set |
| Fan speed | `0x02` | reported only |
| Vane / wide-vane / airflow position | `0x02` | reported only |
| HVAC action (heating/cooling/idle) | `0x06` | derived operating state |

### 9.2 Health & energy sensors
| Sensor | INFO code | Status |
|---|---|---|
| Compressor frequency | `0x06` | Live-verify (outdoor-sourced) |
| Operating stage (`stage_sensor`) | `0x06` | Live-verify |
| Sub-mode / auto sub-mode (`sub_mode_sensor`, `auto_sub_mode_sensor`) | `0x06` | diagnostic |
| Input power (`input_power_sensor`) | `0x09` | Live-verify |
| Energy / kWh (`kwh_sensor`) | `0x09` | Live-verify |
| Runtime hours (`runtime_hours_sensor`) | `0x09` | Live-verify |
| Outside-air temperature | `0x06`/`0x09` | Live-verify (outdoor-sourced) |
| Target humidity (`target_humidity_sensor`) | `0x06` | Live-verify |
| Functions dump (`functions_sensor`) | `0x20`/`0x22` | diagnostic, read-only. Decode raw code/value pairs via the Mitsubishi **[Function Settings List (PDF)](https://www.mitsubishitechinfo.ca/sites/default/files/Function%20Settings%20List.pdf)** (also linked in README §"Hardware Settings"). Availability is gated by the Q5 `0x5b` handshake decision. |
| i-see sensor (`isee_sensor`, binary) | `0x06` | Live-verify (likely absent on PEAD) |

### 9.3 Fault & link diagnostics
| Sensor | INFO code | Notes |
|---|---|---|
| Error / abnormal-state code (`error_code_sensor`) | `0x04` | **High priority.** Decode to P/E/U/F mnemonics per **FR5** |
| Refrigerant-leak alert (`FL`/`FH`/`PL`) | `0x04` | **A2L safety signal** — surface as distinct high-severity binary_sensor (FR5) |
| Connection / link uptime (`hp_uptime_connection_sensor`) | n/a | driver-side health |
| Driver state (BOOT…CONNECTED…DISCONNECTED) | n/a | optional diagnostic text sensor |

---

## 10. Dropped from the Original Codebase (Control Surface)

Everything **removed or disabled** so the device cannot change HVAC state. Grouped by
HA entity platform and by firmware code path. These map to **NFR1–NFR3, NFR5**.

### 10.1 Writable HA entities — removed
| Entity (config key) | Platform | What it controls |
|---|---|---|
| The writable `climate` control path | `climate` | setpoint / mode / fan / swing — replace with read-only or omit |
| `vertical_vane_select`, `horizontal_vane_select`, `airflow_control_select` | `select` (`VaneOrientationSelect`) | vane / airflow direction |
| `air_purifier_switch`, `night_mode_switch`, `circulator_switch` | `switch` (`HVACOptionSwitch`) | run-state toggles |
| `functions_set_button` | `button` (`FunctionsButton`) | writes a function value |
| `functions_set_code`, `functions_set_value` | `number` (`FunctionsNumber`) | function write operands |
| `hardware_settings` | `select` (`HardwareSettingSelect`) | **writes unit hardware config** — highest-risk, must drop |
| `remote_temperature_source`, `remote_temperature_control_sensor` | sensor inputs | inject remote room temp into the unit |

> `functions_get_button` issues only a GET (read). Functions are already polled by the
> scheduler, so it is dropped for simplicity rather than for safety.

### 10.2 Firmware write paths — removed / compiled out
All emit a CN105 `0x41` SET packet; none may remain reachable.

| Function | File | Packet |
|---|---|---|
| `sendWantedSettings()` / `sendWantedSettingsDelegate()` / `createPacket()` | `hp_writings.cpp` | `0x41` settings (power/mode/temp/fan/vane) |
| `sendRemoteTemperature()` / `sendRemoteTemperaturePacket()` | `hp_writings.cpp` | `0x41` remote temp |
| `sendWantedRunStates()` | `hp_writings.cpp` | `0x41` run states (`0x08`) |
| `control()` / `controlDelegate()` / `controlMode/Temperature/Fan/Swing()` | `climateControls.cpp` | (build SET) |
| `set_remote_temperature()` / `set_remote_temp_timeout()` / `set_remote_temp_keepalive_interval()` | `cn105.cpp` | remote-temp watchdog/keepalive |
| `VaneOrientationSelect::control()`, `HVACOptionSwitch` write, `FunctionsNumber/Button` set, `HardwareSettingSelect::control()` | respective `.h/.cpp` | (build SET) |

### 10.3 Config options that become inert
Dropped or ignored because they only shape control behavior: `dual_setpoint`,
`restore_setpoints`, `temperature_margin`, `debounce_delay`,
`remote_temperature_timeout`, `remote_temperature_keepalive_interval`,
`fahrenheit_compatibility` (control-side math). `installer_mode` is retained only insofar
as it affects the read-only handshake choice (§6 Q5).

### 10.4 Explicitly KEPT (not control)
For clarity, these read/transport paths are **retained**: `sendFirstConnectionPacket()`
(`0x5a` handshake), `buildAndSendInfoPacket()` / `createInfoPacket()` (`0x42` INFO polls),
the entire read path (`hp_readings.cpp`, `frame_parser.h`), the `RequestScheduler`, and the
connection FSM.
