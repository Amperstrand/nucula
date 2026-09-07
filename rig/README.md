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

### Token size vs. the emulated area (open question)

A 1-sat cashuB token from the local mint — the smallest possible
hand-off, single proof, DLEQ attached (nucula requires NUT-12) —
measures **366 chars**; with CC + TLV + record overhead the NDEF image
is ~380 bytes. The Write Card Emulation Data StartOffset is one byte,
which conservatively caps the addressable image at 256 bytes. Whether
offsets ≥ 256 actually wrap, error, or address a larger area is
**unverified** — probe it over USB *before* entering CE (no power
cycle burned):

```python
write_ce_data(240, marker)   # then read_ce_data(240, 16)
write_ce_data(280, marker)   # byte overflow or accepted?
```

If the area really caps at 256, Ultralight-CE cannot carry DLEQ
tokens; the pivots are a relay sticker (ACR writes a real NTAG the
atom reads) or NDEF Type 4 over ISO-DEP on the RC522 (NTAG424 bolt
cards as carriers) — the e2e guards already refuse to burn a CE entry
on an oversized image.

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

## Relay e2e runbook

The live money loop (one command once the rig is set up):

```bash
cargo test --features live,payer --test e2e -- --ignored --nocapture
```

Preconditions:

1. **Mint up on its LAN bind** — `/tmp/opencode/rig/start-mint.sh`
   (adapter `0.0.0.0:3338`, FakeWallet, shared-cargo-cache paths).
   The token embeds `http://192.168.13.221:3338`; the atom must be on
   the same LAN (it joins the lab AP automatically).
2. **A writable blank boltcard on the atom's antenna**, within reach
   of the ACR's antenna too (sandwiched; the write goes through the
   ACR's field while the atom's stays off — the firmware gates the
   RC522 antenna to sessions only).
3. **The ACR freshly powered** — if it was wedged (see above), replug
   it; there is no software reset.

The flow: cdk payer mints 1 sat → ACR discovers the card, writes the
NDEF text record into its NDEF file, readback-verifies, quiets its
field → the atom's reader session powers its field, reads the card
over Type 4 / ISO-DEP, extracts the token, swaps it at the mint →
`redeemed` log + balance on the console. The offline variant asserts
the stash-then-drain path instead.

### Leaving a card in the rig (interference)

Electrically, a card parked in the sandwich is inert: the atom's
RC522 antenna is gated OFF at idle (radiates only during reader
sessions), and being polled by a reader is normal life for a card.
The interference concern is *sharing the ACR*: the polling setting is
NVM-backed and global for the reader — while a nucula run has it
quieted, nothing else can see ANY card on that reader (bolty HIL
shares this reader class).

The e2e handles this: a Drop guard restores factory auto-polling on
exit — including on failed assertions — so a finished run always
returns the reader to normal. Between runs the sandwich card is
visible to other users like any card left on a reader; if this ACR is
dedicated to the nucula rig that is fine permanently, otherwise park
the card on the atom only (outside ACR reach) between sessions and
move it into the sandwich for nucula runs.

## Rig hardware map (this lab)

| Role | Device | Path |
|---|---|---|
| DUT | M5Stack Atom + MFRC522 | `/dev/serial/by-id/usb-M5STACK_Inc._M5_Serial_Converter_9D529068B4-if00-port0` |
| Payer tag | ACR1252U PICC | pcscd `ACS ACR1252 Dual Reader [.. PICC] 00 00` |

The other M5 converter (`81528A13B6`) is a second atom without an
MFRC522 attached.
