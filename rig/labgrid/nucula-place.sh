#!/usr/bin/env bash
# Idempotently (re)create the nucula-rig place with its resource matches.
# Coordinator restarts wipe in-memory place definitions — rerun when
# acquire says "matches nothing". Matches EXISTING exporter resources
# (no nucula exporter): the m5stick serial and the reference ACR1252
# live on the ai-legion-nfc bench exporter (/etc/labgrid/exporter-nfc.yaml)
# — acquiring this place excludes the other users of that hardware.
set -euo pipefail

COORDINATOR="${LABGRID_COORDINATOR:-192.168.13.208:20408}"
PLACE="nucula-rig"

lg() { labgrid-client -x "$COORDINATOR" -p "$PLACE" "$@"; }

if lg show >/dev/null 2>&1; then
    echo "place $PLACE exists"
else
    lg create
    echo "place $PLACE created"
fi

# Scope: m5stick + ACR only. No rig atom is wired on this bench — Board::Atom runs
# rely on the host-local flock alone until an atom serial resource is added here
# (the M5STACK device on ttyUSB1 belongs to the micronuts QR rig).
lg add-match 'ai-legion-nfc/m5stick-usb/SerialPort'
lg add-match 'ai-legion-nfc/ref-acr1252/USBDevice'
lg show | sed -n '1,6p'
