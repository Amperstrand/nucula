#!/usr/bin/env bash
# Idempotently (re)create the nucula-rig place with its resource matches.
# Coordinator restarts wipe in-memory place definitions — rerun when
# acquire says "matches nothing". Matches EXISTING exporter resources
# (no nucula exporter): the atom token lives on the microfips bench
# exporter, the ACR1252 token on the bolty exporter — acquiring this
# place excludes both projects from the hardware.
set -euo pipefail

COORDINATOR="${LABGRID_COORDINATOR:-192.168.13.221:20408}"
PLACE="nucula-rig"

lg() { labgrid-client -x "$COORDINATOR" -p "$PLACE" "$@"; }

if lg show >/dev/null 2>&1; then
    echo "place $PLACE exists"
else
    lg create
    echo "place $PLACE created"
fi

lg add-match 'ai-legion-small-microfips/atom-b-serial/BenchSerialToken'
lg add-match 'ai-legion-small-microfips/m5stick-nucula-serial/BenchSerialToken'
lg add-match 'ai-legion-small/acr1252/NetworkSmartcardReader'
lg show | sed -n '1,6p'
