# nucula — agent onboarding

Cashu ecash wallet firmware for ESP32 (XIAO-C3/PN7160 card emulation;
M5Stack Atom and M5StickC Plus + MFRC522 readers) plus the host test
rig (`rig/`) that drives the hardware against a LAN micronuts-mint.
Full picture: `rig/README.md`, `docs/ROADMAP-AND-CONTRIBUTION.md`,
rig plan on GitHub issue Amperstrand/nucula#1.

## The ACR1252U wedge — read before touching the reader

**CONFIRMED TRIGGER (instrumented 2026-09-07): rapid sequential CE-data
writes.** The emulated Type 2 memory is NVM-backed, and the firmware's
CCID loop dies on write bursts — `rig/scripts/acr_instrumented.py`
(the reproducer) showed 48-byte writes 1–3 applying fully at ~142 ms
each, write 4 returning a SHORT length echo (36/48 bytes — a silently
partial write, SW still 9000), and write 5 wedging CCID until physical
replug. One write per power cycle never wedged.

The three earlier e2e burns were blamed on `exit_card_emulation` /
mode-switch races — **wrong by coincidence**: all three died at chunk
4–5 of `present_ndef_image`'s old 6×48-byte write loop, whatever
mode-switch commands surrounded it.

The rules (enforced in `Acr1252::present_ndef_image`):

1. quiet auto-polling (`E0 00 00 23 01 00`) and let it settle 1 s
2. **ONE write command** — trimmed at the NDEF terminator (0xFE),
   ≤ 251 data bytes (the Lc is one byte). No chunked write loops, ever
3. read back and verify
4. enter emulation ONCE (`E0 00 00 40 03 01 00 00`)

That is **one CE scenario per power cycle** — after the scenario, replug
before the next. Don't call `exit_card_emulation` (never was the
trigger, but a fresh reader needs no exit and a CE-stuck one needs a
replug anyway), and don't re-add `set_picc_operating_parameter` to CE
flows — neither is proven harmful, both are unproven-free.

Wedged signature: `Pcsc(NotTransacted)` from an escape, then "Reader is
unavailable". None of these revive it (all tried 2026-09-07): pcscd
restart, sysfs `authorized` toggle, `USBDEVFS_RESET` via sudo, USB port
reset, `set_configuration`, the SAM interface. Physical replug only.

Auto-polling is NVM-backed — it survives replugs holding whatever was
last set. `RigGuard::acquire()`'s preflight restores factory polling
(0x8F) for reader-mode tests; any CE flow after a preflight re-quiets
polling first (present_ndef_image does).

Command semantics reference: ACR1552U Series Reference Manual §6.1.15
(same escape family) — the write response echoes the ACTUAL written
length, so a short echo means a partial write even with SW 9000.

## Other rig gotchas (learned the hard way, same day)

- **M5Stick flash: 115200 baud, EXPLICIT --port, daemon stopped.** Three
  traps, all bit sessions in the field (2026-09-12):
  1. The Hades2001 USB-serial bridge flakes at 460800 (`idf.py flash`
     defaults there) — flash at `-b 115200` with raw esptool.
  2. `@flash_args` carries NO port: esptool then defaults to
     `/dev/ttyUSB0` — on this bench that is the **CYD** (QR-rig source
     board). A "hash verified" flash of the wrong board is silent
     damage. Always pass
     `--port /dev/serial/by-id/usb-Hades2001_M5stack_49D6163EBE-if00-port0`.
  3. The bolty-console systemd daemon holds ttyUSB1 open and eats the
     bootloader sync bytes (esptool: "readiness to read but returned no
     data"; killing it just respawns): `sudo systemctl stop
     bolty-console` for the flash window, start it again after.
  Full spell: `sudo systemctl stop bolty-console && cd build-m5stick &&
  esptool.py --chip esp32 -b 115200 --port
  /dev/serial/by-id/usb-Hades2001_M5stack_49D6163EBE-if00-port0
  --before default_reset --after no_reset write_flash "@flash_args"`,
  then DTR/RTS-reset the chip to boot it.
- **M5Stick build env**: the root (untracked) `sdkconfig` pins
  `esp32c3` — building the m5stick target from a clean tree yields a
  C3 image unless overridden:
  `IDF_TARGET=esp32 SDKCONFIG=build-m5stick/sdkconfig
  SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.m5stick'
  idf.py -B build-m5stick build` (move the root sdkconfig aside if -D
  flags alone don't take).
- **Silent console**: if the M5Stick stops answering (usually stuck in
  download mode after a failed flash), force a normal boot over serial:
  `dtr = False`, pulse `rts` True→False (pyserial), then look for the
  boot banner.
- **RC522 latch-up**: a latched RC522 holds SDA LOW and survives ESP32
  reboots — the M5Stick's AXP192 keeps the Grove rail up across resets,
  and SCL clocking (tried 32 and 128 pulses) clears it only
  transiently. The firmware power-cycles the rail via AXP192 EXTEN
  between setup retries (`axp192_grove_power` in `display_st7789.c`).

## Commits / PRs

Branches on Amperstrand/nucula only. Never file PRs or issues upstream
(zeugmaster/nucula) without human review — see
`docs/ROADMAP-AND-CONTRIBUTION.md`.
