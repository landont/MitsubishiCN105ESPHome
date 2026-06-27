# PEAD Health Monitor — Development Plan

*Companion to [`MONITOR_REQUIREMENTS.md`](./MONITOR_REQUIREMENTS.md). Status: plan / no code written yet.*
*Strategy: compile-time `monitor_only` mode on top of this codebase (Requirements §5, option 1),*
*sequenced for a **single physical install trip** with all subsequent iteration done over OTA.*

---

## The one-trip constraint (drives everything)

The device installs in the **attic** — hard to reach — and we go up there **once**. After
that, all validation, sensor pruning, decoder tuning, and the Q2 multi-master experiment
happen **remotely over OTA/WiFi**. This reshapes the original plan in three ways:

1. **No bench/bus validation step before the trip.** We can't pre-validate Q2 (multi-master
   safety) on the bench because the live PEAD bus is only at the install site. Instead, the
   firmware we carry up boots in the **safest mode** and we validate Q2 *in situ, remotely.*
2. **Passive ↔ active becomes a runtime toggle, not a re-flash.** The installed image must
   contain *both* capabilities — passive RX-only **and** active polling — with **passive as
   the boot default**. We arm active polling over the air to test Q2, and disarm it over the
   air if it misbehaves. No trip, no re-flash to change modes. *(The `monitor_only`
   no-`0x41`-ever guarantee stays compile-time — only polling aggressiveness is runtime.)*
3. **Losing WiFi/OTA = a second trip.** So OTA resilience and attic WiFi coverage are now
   *hard, blocking* requirements, not nice-to-haves. Everything reachability-related must be
   proven on the bench and re-confirmed before we leave the attic.

> Good side effect: if Q2 turns out false, we don't pivot to a different product or make a
> return trip — the same installed firmware just stays in passive mode permanently.

---

## New requirements implied by the constraint

These extend `MONITOR_REQUIREMENTS.md` (propose folding back into it):

- **NFR6 — Runtime safety-mode toggle.** Passive RX-only vs active polling (`0x5a`/`0x42`)
  is a **runtime** flag exposed to HA, **defaulting to passive on every boot**. Active
  polling is *armed*, never the cold-start default. `monitor_only` (no `0x41`) remains
  compile-time and immutable.
- **NFR7 — Active-mode dead-man.** When active polling is armed, it auto-reverts to passive
  unless periodically re-armed (or after N bus anomalies). A power cycle always lands safe.
- **NFR8 — OTA must survive bad flashes.** ESPHome `safe_mode` + OTA rollback + WiFi **AP
  fallback** configured, so a bad image or a dropped network self-recovers without physical
  access. Bus-polling state must never be able to take down WiFi/OTA (they're independent).
- **NFR9 — Attic WiFi margin.** Verified RSSI headroom at the install location; if marginal,
  remediated during the one trip (the only thing genuinely un-fixable remotely).

---

## Stage A — Bench build (before the trip; no attic, no live bus)

**Goal:** produce one "install image" that is safe on boot, remotely recoverable, streams
diagnostics, and contains every capability we'll need later — because we can't add hardware
access back. Everything here is doable without the real PEAD.

| Step | Task | File / artifact | Maps to |
|---|---|---|---|
| A1 | `monitor_only` compile flag in schema + the `writePacket()` **allow-list guard** (drop any packet ∉ `{0x5a,0x42}`, log ERROR, count `blocked_write_count_`). Unit-test the guard. | `climate.py`, `cn105.h`, `hp_writings.cpp`, `tests/unit/` | NFR2 |
| A2 | Compile-out / gate all SET paths behind `!monitor_only`: `sendWantedSettings*`/`createPacket`, `sendRemoteTemperature*` + remote-temp watchdog/keepalive, `sendWantedRunStates`, `control()`/`controlDelegate`. | `hp_writings.cpp`, `climateControls.cpp`, `cn105.cpp` | NFR1, NFR5 |
| A3 | Read-only HA surface: skip all writable platforms (select/switch/number/button/hardware_settings/remote-temp); keep `sensor`/`binary_sensor`/`text_sensor`; distinct `pead_monitor_*` prefix; climate entity omitted or read-only no-op. | `climate.py` | NFR3, FR3, FR4 |
| A4 | **`0x04` decoder + A2L leak alert** (P/E/U/F mnemonics; `FL`/`FH`/`PL` → distinct high-severity binary_sensor). Fully unit-tested — the A2L path can't be triggered on real hardware, so tests are the proof. | `error_code_sensor.h`, `hp_readings.cpp:447`, `tests/unit/` | FR5 |
| A5 | **Runtime passive/active toggle (NFR6)** + dead-man auto-revert (NFR7). HA switch/`button` to arm active; passive on boot; auto-disarm on anomaly or missed re-arm. | `cn105.h`, scheduler, `climate.py` | NFR6, NFR7 |
| A6 | **OTA resilience (NFR8):** `safe_mode`, OTA platform + rollback, WiFi AP fallback / captive portal. **Prove it on the bench:** deliberately OTA a broken image and confirm self-recovery; pull the network and confirm reconnect. | YAML | NFR8 |
| A7 | **Power-chain bench test:** drive the ESP32-S3 from a 12 V bench supply through the *intended* buck converter, stress WiFi TX, confirm no brownout. **Decide final wiring now** (12 V ACC leg vs external supply) — it's wired once and not revisited. | hardware + notes | Q3 |
| A8 | **Remote byte-capture/log streaming:** the passive `FrameParser` capture is a permanent diagnostic that streams over WiFi/HA, so Q2 analysis and code-enumeration happen from your desk. | logging path | FR3 live-verify, §8 |

**Exit gate (must all hold before the trip):** install image boots **passive**; OTA
self-recovers from a bad flash on the bench; power chain validated under WiFi load; guard +
decoder unit-tested; capture streams remotely; CI green (add a `monitor_only` example to the
build matrix so the mode can't bit-rot).

---

## Stage B — The install trip (once, in the attic)

Purely physical + reachability confirmation. **No Q2 experimentation here** — leave passive.

| Step | Task | Maps to |
|---|---|---|
| B1 | Mount the ESP32-S3; wire power per the A7 decision; connect to the splitter **ACC** port. | §2, Q3 |
| B2 | Power on; confirm: on WiFi, visible in HA, **passive capture streaming**, and **OTA works** — push a no-op OTA from your phone *before leaving* to prove the remote channel end-to-end. | NFR8, §8 |
| B3 | **Confirm attic WiFi RSSI margin (NFR9).** This is the make-or-break for the whole one-trip premise and the *only* thing not fixable remotely. If marginal: reposition / add antenna / place a repeater **now**. | NFR9 |
| B4 | Leave it in **passive** mode. Come down. You do not return. | NFR6 |

**Exit:** device installed, reachable, OTA-proven, streaming passively.

---

## Stage C — Remote phase (OTA only, from your desk)

Everything that needs the live bus, done over the air.

| Step | Task | Maps to |
|---|---|---|
| C1 | **Q2 validation, remotely.** Arm active polling (A5 toggle); issue a single `0x5a`+`0x42`; watch the streamed capture **and** Airzone behavior for NAK/retries/zone disruption. Escalate cadence only if clean. If it disrupts → disarm (reverts to passive), **no trip**. Pick handshake `0x5a` default, `0x5b` only if refused (Q5). | Q2, Q5, Risk §7 |
| C2 | **Live sensor pruning** over OTA: from the capture, keep codes returning populated `0x62` data; let `maxFailures` auto-disable the outdoor/M-series-leaning ones (compressor freq, power/kWh, outdoor temp, i-see). Demote unconfirmed sensors from "guaranteed." | Q4, FR3, §9 |
| C3 | Decoder/sensor/config refinement; OTA the finalized build. | FR3, FR5 |
| C4 | **Acceptance verification (§8), all remote:** long capture shows only `0x5a`/`0x42` and `blocked_write_count_==0`; Airzone retains control; no writable entity in HA; survives forced disconnect/reconnect cycles write-free. | §8 |

---

## Cross-cutting

- **Reachability is now sacred.** Bus-mode state is independent of WiFi/OTA by design, so an
  aggressive-polling mistake can always be reverted over the air. The only true brick risks
  are (a) a bad image that breaks WiFi — handled by safe_mode + rollback (A6), and (b) weak
  attic WiFi — handled at B3. Both are addressed before the trip ends.
- **Testing:** CI stays green every step; NFR2 guard (A1) and FR5 decoder (A4) get dedicated
  unit tests; the `monitor_only` example joins the 6-config build matrix.
- **Power is irreversible.** A7's wiring decision is made on the bench and committed in B1;
  if unsure, wire the safer external supply rather than betting on the 12 V ACC leg.
- **Docs hygiene:** correct `PEAD-A24AA7` → **PEAD-AA24NL** (Req §6 Q4); keep the board photo
  out of the committed tree.

---

## Sequencing summary

```
STAGE A  (bench, pre-trip)   build the single safe, OTA-recoverable install image
  A1 guard ─ A2 strip writes ─ A3 read-only UI ─ A4 0x04 decoder ─
  A5 passive/active runtime toggle ─ A6 OTA resilience ─ A7 power ─ A8 remote capture
        │  exit gate: boots passive, OTA self-recovers, power & WiFi proven on bench
        ▼
STAGE B  (attic, ONCE)       mount · wire · connect ACC · prove WiFi+OTA · leave passive
        │  never return
        ▼
STAGE C  (remote, OTA)       Q2 arm/observe ─ sensor pruning ─ refine ─ acceptance §8
```
