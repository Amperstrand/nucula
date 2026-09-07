# nucula — agent onboarding

Cashu ecash wallet firmware for ESP32 (XIAO-C3/PN7160 card emulation;
M5Stack Atom and M5StickC Plus + MFRC522 readers) plus the host test
rig (`rig/`) that drives the hardware against a LAN micronuts-mint.
Full picture: `rig/README.md`, `docs/ROADMAP-AND-CONTRIBUTION.md`,
rig plan on GitHub issue Amperstrand/nucula#1.

## The ACR1252U wedge — read before touching the reader

The ACR1252U runs CCID and NFC on one MCU. A card-emulation
**mode-switch escape** (`E0 00 00 40 03 …`) issued in the wrong state
wedges its CCID loop until the reader is **physically unplugged**. It
has wedged every rig run that included `exit_card_emulation` — from CE
mode AND from a fresh post-replug reader — plus the original CE
experiments. A 1 s settle after quieting polling does not save it; the
exit escape itself is the trigger.

**Never call `Acr1252::exit_card_emulation`.** A fresh reader needs no
exit, and a reader left in CE mode needs a replug anyway.

The quiet→write→verify→enter sequence above is proven ONLY when driven
manually (python `pyscard`, `T0_protocol` + `DIRECT`, one connection,
single small write, no preflight). The same bytes from the rig test
(pcsc crate, `UNDEFINED` + `DIRECT`, preflight connection churn first,
6×48-byte chunked writes) wedged the reader three times on 2026-09-07 —
`NotTransacted` at `present_ndef_image` every time. The open suspects,
narrowed by elimination: connect-protocol difference, preflight
connection drop/reopen, or the chunked 48-byte write pattern.
`rig/scripts/acr_instrumented.py` replicates the full failing context
over the proven transport with per-step logging — run it on the next
fresh power cycle to isolate the trigger BEFORE burning another e2e
attempt.

The manual sequence that worked (and which the instrumented script
replays):

1. quiet auto-polling (`E0 00 00 23 01 00`) — before anything else
2. write the CE image — ONE small (~30-byte) write in the manual run;
   the rig's 6×48-byte chunking is an open wedge suspect
3. read back and verify
4. enter emulation ONCE (`E0 00 00 40 03 01 00 00`)

That is **one CE scenario per power cycle** — after the scenario, replug
before the next. `set_picc_operating_parameter` inside a CE flow is also
suspect (present in both wedged runs, absent from the working one);
don't re-add it without a replug available to burn.

Wedged signature: `Pcsc(NotTransacted)` from an escape, then "Reader is
unavailable". None of these revive it (all tried 2026-09-07): pcscd
restart, sysfs `authorized` toggle, `USBDEVFS_RESET` via sudo, USB port
reset, `set_configuration`, the SAM interface. Physical replug only.

Auto-polling is NVM-backed — it survives replugs holding whatever was
last set. `RigGuard::acquire()`'s preflight restores factory polling
(0x8F) for reader-mode tests; any CE flow after a preflight must
re-quiet polling before its mode switch (present_ndef_image does).

## Other rig gotchas (learned the hard way, same day)

- **M5Stick flash: 115200 baud.** The Hades2001 USB-serial bridge flakes
  at 460800 (syncs, then "No serial data received"). `idf.py flash`
  defaults to 460800 — flash with raw esptool instead:
  `python -m esptool --chip esp32 -b 115200 --before default_reset
  --after no_reset write_flash "@flash_args"` from `build-m5stick/`,
  then DTR/RTS-reset the chip to boot it.
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
