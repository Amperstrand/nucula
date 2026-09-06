# nucula rig

Host-side test rig for the M5Stack Atom + MFRC522 port: drives the
ACR1252U (payer-side tag emulation), the atom's console over serial,
and — once wired — mints real tokens at a local `micronuts-mint` to
hand over the air.

```
cargo test                     # pure-logic unit tests (no hardware)
cargo build --features live    # compile hardware paths
cargo test  --features live -- --ignored   # live e2e (ACR + atom)
```

## ACR1252U findings (all established empirically, fw `ACR1252U_V101.0`)

### The escape path

- Reader-specific `E0` commands go through PC/SC `SCardControl` with
  **`SCARD_CTL_CODE(1)`** (`0x42000001` on pcsclite 2.x — the formula
  is `0x42000000 + code`, no shift, unlike older guides).
- The escape ioctl only appears in the feature list when libccid's
  `ifdDriverOptions` has bit 0 set (`0x0003` in
  `/usr/lib/pcsc/drivers/ifd-ccid.bundle/Contents/Info.plist`), then a
  pcscd restart; the feature is tag `0x13` (FEATURE_CCID_ESC_COMMAND).
- Open the PICC interface in **DIRECT share mode** — no card in the
  field needed.
- Escape responses are framed `E1 00 00 00 <len> <payload>`.

### Card emulation (MIFARE Ultralight, NFCMode 01)

- The emulated tag serves the **reader's own UID in pages 0–2**; the
  preloaded data area is served **starting at page 3**. A T2T NDEF
  image must therefore be **CC + TLV only** (no UID/BCC/lock pages) —
  verified against the atom's `nfcdump`.
- `Write`/`Read Card Emulation Data` take a **one-byte StartOffset**;
  addressability beyond offset 255 is unverified — keep images small.
- Reader NVM settings (tag-type operating parameter, auto-polling)
  **survive replugs** — and with all tag types muted (`0x00`) **the
  card emulation does not radiate at all**. `present_ndef_image`
  restores factory settings (`0xFF` / `0x8F`) before entering CE.

### The CE one-way door

After entering card emulation, the reader **stops answering every USB
CCID request** (all escapes time out; pcscd marks it unavailable).
None of these revive it — only unplugging the reader:

- USB port reset (`libusb` `dev.reset()`)
- `set_configuration` toggle
- the SAM slot's independent CCID interface
- draining the interrupt endpoint
- waiting (no timeout observed)
- sysfs `authorized` 0→1 toggle (re-enumerates the device; the CCID
  stack stays wedged, and it broke pcscd until the physical replug)

Consequence for the rig: **one CE scenario per power cycle**. Enter CE
only when everything else (image, atom session) is already set up.

### MFRC522 side notes (atom)

- The RC522 can latch with SDA held low (observed after host-side
  watchdog resets mid-I2C-scan). Recovery per NXP AN10217 (SCL
  clocking + STOP) clears it; if the reader's own field is what
  sustains the latch, quieting it first helps. The firmware runs
  recovery at boot and exposes `i2crecover <clocks>`.
- The atom is `board-m5atom` Grove wiring: SDA 26 / SCL 32, I2C
  address `0x28` (same rig as ccid-firmware-rs / bolty-rs).
- Do not stack other cards between the antennas: a boltcard between
  the RC522 and the ACR shields the emulated tag completely (the
  reader happily selects the boltcard instead).

## Rig hardware map (this lab)

| Role | Device | Path |
|---|---|---|
| DUT | M5Stack Atom + MFRC522 | `/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0` |
| Payer tag | ACR1252U PICC | pcscd `ACS ACR1252 Dual Reader [.. PICC] 00 00` |

The other M5 converter (`81528A13B6`) is a second atom without an
MFRC522 attached.
