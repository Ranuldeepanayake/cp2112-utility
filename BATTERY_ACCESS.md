# Battery Access Reference

This document covers the I2C/SMBus communication protocols, register maps, and
security model used by the TI BQ-series battery pack managers supported in this
project (bq30z554-R1, BQ4050, BQ40Z60).

---

## Protocols

### SBS Word Reads

All three devices implement Smart Battery System (SBS) v1.1 word commands.
The host writes a 1-byte register address; the device responds with 2 bytes,
little-endian.

| Register | Name | Notes |
|----------|------|-------|
| 0x03 | BATTERY_MODE | Bit 15 = CAPM (capacity in 10mWh vs mAh) |
| 0x08 | TEMPERATURE | 0.1K units; subtract 273.15 for °C |
| 0x09 | VOLTAGE | mV |
| 0x0A | CURRENT | mA, signed int16 |
| 0x0B | AVERAGE_CURRENT | mA, signed int16 |
| 0x0D | RELATIVE_STATE_OF_CHARGE | % |
| 0x0F | REMAINING_CAPACITY | mAh or 10mWh depending on CAPM |
| 0x10 | FULL_CHARGE_CAPACITY | mAh or 10mWh |
| 0x16 | BATTERY_STATUS | Status and alarm flags |
| 0x17 | CYCLE_COUNT | Full charge cycles |

### Direct Block Reads (0x50–0x57, 0x71–0x76)

TI BQ devices expose 32-bit status registers and extended data blocks as
SMBus block reads at fixed addresses. The response wire format is:

```
[length_byte] [data byte 0] [data byte 1] ... [data byte N-1]
```

`length_byte` must equal the expected payload length; the driver rejects
responses where it does not match. All multi-byte fields within the payload
are little-endian.

**Status registers (all three devices — 0x50–0x57, 4-byte payload):**

| Register | Name |
|----------|------|
| 0x50 | SAFETY_ALERT |
| 0x51 | SAFETY_STATUS |
| 0x52 | PF_ALERT (permanent fail) |
| 0x53 | PF_STATUS |
| 0x54 | OPERATION_STATUS |
| 0x55 | CHARGING_STATUS |
| 0x56 | GAUGING_STATUS |
| 0x57 | MANUFACTURING_STATUS |

**BQ4050: status registers accessed via MAC (not direct).** The BQ4050 does
not support direct block reads at 0x50–0x57. These registers are reached
through MAC subcommands 0x0050–0x0057 using the standard 0x00/0x23 MAC
protocol instead.

**Extended data registers (BQ40Z60 only — 0x71–0x76, 32-byte payload):**

| Register | Content |
|----------|---------|
| 0x71 | DAStatus1: cell1–4 voltages (mV), BAT/PACK voltages, cell1–4 currents (mA), cell1–4 power (cW), pack/avg power (cW) |
| 0x72 | DAStatus2: internal, TS1–4, cell, FET temperatures (0.1K each) |
| 0x73 | GaugeStatus1: TrueRemQ/E, InitQ/E, ReserveQ/E, T_sim/ambient, RaScale, CompRes per cell |
| 0x74 | GaugeStatus2: PackGrid, LStatus, CellGrid, StateTime, DOD0/DODEOC per cell |
| 0x75 | GaugeStatus3: QMax per cell, QMaxDOD0, QMaxPassedQ, QMaxTime, TempK/A |
| 0x76 | CBStatus: cell balance time per cell (seconds) |

On the BQ4050 these same data blocks are reached through MAC subcommands
0x0071–0x0072 (see below).

### ManufacturerAccess (MAC) — Standard Protocol

All three devices share the standard SBS MAC protocol:

1. Write 3 bytes to register **0x00** (ManufacturerAccess):
   `[0x00, subcommand_low, subcommand_high]`
2. Read N+1 bytes from register **0x23** (ManufacturerData):
   response is `[length, data0, data1, ..., dataN-1]`

This is the only mechanism used for data reads. It works for every
subcommand that returns a response payload, regardless of security level
(provided the subcommand is accessible in the current security mode).

### ManufacturerBlockAccess (0x44)

A second MAC variant exists for **write-with-payload** operations (data
flash writes, SecurityKeys changes). The entire transaction is carried in a
single block write/read on register **0x44**:

```
Write: [0x44, total_length, subcommand_low, subcommand_high, payload...]
Read:  [0x44, total_length, subcommand_low, subcommand_high, response...]
```

**Protocol capability comparison:**

| Operation | 0x00/0x23 | 0x44 |
|-----------|:---------:|:----:|
| Read status block (e.g. DAStatus1) | ✓ | ✓ |
| Read data flash value | ✓ | ✓ |
| Write data flash value | ✗ | ✓ |
| Write security keys | ✗ | ✓ |
| Trigger a no-data command (e.g. seal, reset) | ✓ | ✓ |

Used by the `write-mac` action on BQ4050 and BQ40Z60. The bq30z554-R1 does not
support 0x44 — it uses an older 0x22/0x20 row-write protocol for data flash writes.

---

## Security Model

### Modes

All three devices have three security levels:

| Mode | Description |
|------|-------------|
| **SEALED** | Normal operating state. Status/gauging reads work; write operations and some read subcommands are blocked. |
| **UNSEALED** | Configuration reads enabled; some write operations allowed. |
| **FULL ACCESS** | All operations enabled including data flash write and SecurityKeys change. |

### SEC Bit Encoding

The current mode is reported in **OPERATION_STATUS bits [9:8]** (SEC[1:0]).
**The encoding differs between devices:**

| SEC[1:0] | bq30z554-R1 | BQ4050 | BQ40Z60 |
|----------|-------------|--------|---------|
| `00` | Reserved | Reserved | Reserved |
| `01` | **Unsealed** | **Full Access** | **Unsealed** |
| `10` | **Full Access** | **Unsealed** | **Full Access** |
| `11` | **Sealed** | **Sealed** | **Sealed** |

The BQ4050 encoding is inverted relative to the other two devices. Each
driver decodes independently.

### Reading Security Status

| Device | How OPERATION_STATUS is read |
|--------|------------------------------|
| bq30z554-R1 | Direct block read at 0x54 (4 bytes) |
| BQ4050 | MAC subcommand 0x0054 via 0x00/0x23 |
| BQ40Z60 | Direct block read at 0x54 (4 bytes) |

### Transition Procedures

#### bq30z554-R1 (SHA-1 HMAC)

The bq30z554-R1 uses **SHA-1 HMAC challenge-response authentication** for
both SEALED→UNSEALED (MAC 0x0031) and UNSEALED→FULL ACCESS (MAC 0x0032)
transitions. A full SHA-1 engine is required. This is not supported by the
current driver; `enter-unsealed` and `enter-full-access` actions return an
error message. See SLUU511 §10.5.

#### BQ4050 and BQ40Z60 (two-word key write)

Write **word1** then **word2** to ManufacturerAccess (0x00), each as a
3-byte SMBus write. Both writes must complete within 4 seconds.

```
Write: [0x00, key_word1_low, key_word1_high]
Write: [0x00, key_word2_low, key_word2_high]
```

- **SEALED → UNSEALED**: write the unseal key pair
- **UNSEALED → FULL ACCESS**: write the full access key pair
- **Any mode → SEALED**: MAC 0x0030 (no key required, device must be
  UNSEALED or FULL ACCESS)

#### Default Factory Keys (BQ4050 and BQ40Z60)

| Key | Default Value |
|-----|--------------|
| Unseal word 1 | `0x0414` |
| Unseal word 2 | `0x3672` |
| Full Access word 1 | `0xFFFF` |
| Full Access word 2 | `0xFFFF` |

Source: SLUUAQ3A §10.5 (BQ4050) and SLUUA04D §10.5 (BQ40Z60).

> **Security note:** The factory default keys are public. Change them in Full
> Access mode using the SecurityKeys subcommand (MAC 0x0035, requires
> ManufacturerBlockAccess 0x44 for the write). Store the updated keys in
> `devices.ini` (see Key Configuration below).

### Key Configuration

Security keys are stored per-device in `config/devices.ini`:

```ini
[device BQ4050-1]
unseal_key_1=0x0414
unseal_key_2=0x3672
full_access_key_1=0xFFFF
full_access_key_2=0xFFFF
```

Key resolution priority (highest first):

1. Action parameters — two hex words passed at call time override everything
2. Config file values — `unseal_key_1/2` or `full_access_key_1/2`
3. Driver default — factory values hardcoded in the driver

A config value of `0` for both words of a pair is treated as "not set" and
falls through to the driver default.

---

## Device Comparison

| Feature | bq30z554-R1 | BQ4050 | BQ40Z60 |
|---------|-------------|--------|---------|
| **SBS words (0x03–0x17)** | Direct word | Direct word | Direct word |
| **Status flags (0x50–0x57)** | Direct block | MAC 0x0050–0x0057 | Direct block |
| **Per-cell voltages** | MAC 0x0071 (12 B) | MAC 0x0071 (32 B) | Direct 0x71 (32 B) |
| **Per-cell currents** | — | MAC 0x0071 (32 B) | Direct 0x71 (32 B) |
| **Per-cell power** | — | MAC 0x0071 (32 B) | Direct 0x71 (32 B) |
| **Temperatures** | MAC 0x0072 (14 B) | MAC 0x0072 (14 B) | Direct 0x72 (14 B) |
| **IT gauging state** | MAC 0x0073–0x0074 | — | Direct 0x73–0x75 |
| **Cell balance times** | — | — | Direct 0x76 (32 B) |
| **State of Health** | — | — | MAC 0x0077 (4 B) |
| **Security mode read** | Direct 0x54 | MAC 0x0054 | Direct 0x54 |
| **Unseal mechanism** | SHA-1 HMAC (MAC 0x0031) | Two-word key write | Two-word key write |
| **Default unseal key** | N/A | 0x0414 / 0x3672 | 0x0414 / 0x3672 |
| **Default FA key** | N/A | 0xFFFF / 0xFFFF | 0xFFFF / 0xFFFF |
| **Seal command** | MAC 0x0030 | MAC 0x0030 | MAC 0x0030 |
| **Device reset** | MAC 0x0012 | — | MAC 0x0041 |
| **LED test** | — | MAC 0x002B / 0x002C | MAC 0x002B / 0x002C |
| **SEC[1:0]=01** | Unsealed | Full Access | Unsealed |
| **SEC[1:0]=10** | Full Access | Unsealed | Full Access |
| **SEC[1:0]=11** | Sealed | Sealed | Sealed |

---

## Per-Device Register Summary

### bq30z554-R1

| Read type | How | Source |
|-----------|-----|--------|
| SBS words | Direct word read | 0x03–0x17 |
| Status flags | **Direct block read** | 0x50–0x57 (4 B each) |
| Per-cell voltages | MAC 0x0071 | 0x00/0x23 (12 B) |
| Temperatures | MAC 0x0072 | 0x00/0x23 (14 B) |
| IT gauging state 1 | MAC 0x0073 | 0x00/0x23 (30 B) |
| IT gauging state 2 | MAC 0x0074 | 0x00/0x23 (30 B) |
| Security mode | Direct block read | 0x54 bits [9:8] |

### BQ4050

| Read type | How | Source |
|-----------|-----|--------|
| SBS words | Direct word read | 0x03–0x17 |
| Status flags | **MAC subcommand** | 0x0050–0x0057 via 0x00/0x23 (4 B) |
| Per-cell voltages, currents, power | MAC 0x0071 | 0x00/0x23 (32 B) |
| Temperatures | MAC 0x0072 | 0x00/0x23 (14 B) |
| Security mode | MAC 0x0054 | 0x00/0x23 bits [9:8] |

The critical difference from BQ30Z55/BQ40Z60: status registers are not
directly addressable; they must be fetched via the 0x00/0x23 MAC path.

### BQ40Z60

| Read type | How | Source |
|-----------|-----|--------|
| SBS words | Direct word read | 0x03–0x17 |
| Status flags | **Direct block read** | 0x50–0x57 (4 B each) |
| Per-cell voltages, currents, power | **Direct block read** | 0x71 (32 B) |
| Temperatures | **Direct block read** | 0x72 (14 B) |
| IT gauging state 1–3 | **Direct block read** | 0x73–0x75 (32 B each) |
| Cell balance times | **Direct block read** | 0x76 (32 B) |
| State of Health (FCC + energy) | MAC 0x0077 | 0x00/0x23 (4 B) |
| Security mode | Direct block read | 0x54 bits [9:8] |

The BQ40Z60 exposes the most data via direct block reads. MAC is only
needed for the SoH subcommand and for write operations.

---

## Available Actions

| Action | BQ30Z55 | BQ4050 | BQ40Z60 |
|--------|---------|--------|---------|
| `security-status` | Yes | Yes | Yes |
| `seal` | Yes | Yes | Yes |
| `enter-unsealed` | No (SHA-1 required) | Yes | Yes |
| `enter-full-access` | No (SHA-1 required) | Yes | Yes |
| `read-mac <subcommand> <length>` | Yes | Yes | Yes |
| `write-mac <subcommand> <byte...>` | No (0x44 not supported) | Yes | Yes |
| `device-reset` | Yes (MAC 0x0012) | No | Yes (MAC 0x0041) |
| `led-toggle` | No | Yes | Yes |
| `led-press` | No | Yes | Yes |

### `read-mac` usage

Reads any MAC subcommand response after unsealing. Takes two parameters:
the subcommand in hex and the expected response length in bytes (1–32).

```
read-mac 0x0035 8     # SecurityKeys (requires Full Access)
read-mac 0x0054 4     # OperationStatus (readable in any mode)
read-mac 0x0071 32    # DAStatus1 (BQ4050/BQ40Z60)
```

For 2-byte and 4-byte responses the output also shows the little-endian
word/dword value and its decimal equivalent.

> The length must match the subcommand's actual response size. If the device
> NACKs or returns a mismatched length byte the action reports failure with
> a hint to verify the security mode.

### `write-mac` usage

Writes data to a MAC subcommand via ManufacturerBlockAccess (0x44). Takes the
subcommand in hex followed by one or more data bytes in hex (1–30 bytes).
Device must be in **UNSEALED** or **FULL ACCESS** mode first.

**Multi-byte input:** the interactive prompt collects the entire data payload as a
single entry. Enter all data bytes space-separated on one line:

```
Subcommand (hex): 0x4605
Data byte(s) (hex, 1–30 bytes): 0x00 0x00
```

```
Subcommand (hex): 0x0035
Data byte(s) (hex, 1–30 bytes): 0x14 0x36 0x72 0x04 0xFF 0xFF 0xFF 0xFF
```

Each token is parsed independently as a hex value (0x prefix optional). Up to
30 bytes may be entered on one line.

> **NVM timing:** data flash writes trigger internal NVM programming that takes
> 50–100 ms. The driver returns immediately after the I2C transaction completes.
> Wait at least 100 ms before issuing a `read-mac` to verify — reading too soon
> may return stale data or receive a NACK.

> **Warning:** writes go directly to device data flash. An incorrect subcommand
> address or wrong value can corrupt gauging calibration. Always read back with
> `read-mac` to verify, and confirm the subcommand address and data format from
> the device TRM before proceeding.

### Typical data flash write workflow

```sh
# 1. Unseal the device
device BQ4050-1 enter-unsealed

# 2. Confirm the security mode before writing
device BQ4050-1 security-status

# 3. Write the value (subcommand address and data bytes from the TRM)
device BQ4050-1 write-mac 0x4605 0x00 0x00

# 4. Read back to verify the write took effect
device BQ4050-1 read-mac 0x4605 2

# 5. Seal when done
device BQ4050-1 seal
```

The same pattern applies to BQ40Z60 — substitute the device name and subcommand
address from the respective TRM.

**BQ30Z55 note:** `write-mac` is **not available** on the bq30z554-R1. That device
uses an older data flash write protocol (registers 0x22/0x20, not 0x44) that is not
supported by this driver. Use `read-mac` for privileged reads if the device is already
in UNSEALED or FULL ACCESS mode (SHA-1 HMAC required to get there — see §10.5 of
SLUU511).
