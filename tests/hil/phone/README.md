# Phone-vs-device HIL suites (nucula)

`tests/hil/phone/` — pytest + labgrid driving the bench Samsung S20 FE
(cashu.me) against nucula wallet firmware on the ESP32-C3 bench board.
Design + topology: `docs/HIL-PHONE-DESIGN.md`.

## Running (bench host or the `[self-hosted, bench]` runner)

```bash
# all suites, live hardware:
python3 -m pytest tests/hil/phone/ -v --hil \
    --timeout=600 --timeout-method=signal --junitxml=hil-junit.xml

# one suite:
python3 -m pytest tests/hil/phone/test_t1_nucula_selftest.py -v --hil

# dev/CI without hardware: omit --hil (every test skips)
python3 -m pytest tests/hil/phone/ -v
```

`--mint-url` (default `https://testnut.cashu.space`, fake sats) selects
the mint the rig payer funds from. Evidence lands in `hil-results/<ts>/`
(console transcripts, uiautomator dumps, NDEF payloads) — committed
runs are tagged by CI artifacts instead.

## The ccid-broker swap dance (nucula-C3)

The bench C3 normally serves the ccid-firmware-rs GemPC image behind
`/opt/bench/nucula-ccid-broker.py` (systemd `nucula-ccid-broker`,
pcscd reader "Nucula CCID via broker"). Wallet-firmware sessions swap
it OUT and restore byte-exact afterwards — full procedure in
`micronuts docs/PHONE-BRINGUP-RUNBOOK.md` lineage /
`/root/src/worktrees/RESTORE-ECASH-RUNBOOK-v2-20261021.md` pattern:

```bash
# 0) under the labgrid place (acquire nucula-c3 first!)
esptool --chip esp32c3 -b 115200 -p /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_90:DA:72:9A:50:18-if00 \
    read-flash 0x0 0x400000 nucula-ccid-backup-$(date +%Y%m%d-%H%M).bin
systemctl stop nucula-ccid-broker          # it holds the CDC forever
# 1) flash wallet fw (IDF v5.5.5; wifi_config.h from the example, gitignored)
# 2) run the suites
# 3) restore: write-flash the backup slices (or full image), then
systemctl start nucula-ccid-broker
python3 -c 'from smartcard.System import readers; \
    print([str(r) for r in readers() if "Nucula CCID" in str(r)])'
```

## Suite-specific bench facts

- **T0/T1**: need only the coordinator + the wallet firmware on the C3.
- **T2**: wallet fw must carry bench WiFi creds (`main/wifi_config.h`,
  gitignored; template `wifi_config.example.h`) and reach the mint.
- **T3**: the phone must sit on the ACR1252 antenna; SELECT-only reads.
- **T4**: the phone must sit on the nucula PN7160 antenna. The phone
  initiates the read (safe direction); one payment scenario per run.
- **ACR1252U wedge rule** (2026-09-07): NEVER write CE-data through the
  ACR — one CE scenario per power cycle, reads are safe. T3 is
  read-only by construction; keep it that way.

## Skipping is evidence

Every skip carries its reason (place holder, board image, antenna
geometry). A skipped stage is a recorded bench fact, not a failure to
hide — and never a reason to fake a tap.
