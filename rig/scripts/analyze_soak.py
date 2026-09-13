#!/usr/bin/env python3
"""Analyze soak campaign JSONL results.

Reconstructs the campaign timeline across supervised slices: per-slice
cycle/fail tallies, the wallet balance series (from cycle events), and
every balance DECREASE — the wallet-state-reset boundaries the
crash-consistency forensics hinges on — with the nearest preceding
fail/abort events.

    python3 rig/scripts/analyze_soak.py [results_dir]
"""

import json
import sys
from pathlib import Path


def main() -> int:
    results = Path(sys.argv[1] if len(sys.argv) > 1 else
                   Path(__file__).resolve().parent.parent / "results")
    files = sorted(results.glob("soak-*.jsonl"),
                   key=lambda p: p.stat().st_mtime)
    if not files:
        print(f"no soak-*.jsonl under {results}")
        return 1

    balance_series = []  # (ts, slice, balance, source_event)
    decreases = []
    slices = []
    cur = {"file": None, "cycles": 0, "fails": 0, "aborts": 0}
    last_balance = None
    last_events = []  # rolling window of recent event kinds for context

    for f in files:
        cur = {"file": f.name, "cycles": 0, "fails": 0, "aborts": 0}
        for line in f.read_text(errors="replace").splitlines():
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = ev.get("kind", "")
            if kind == "cycle":
                cur["cycles"] += 1
                bal = ev.get("balance")
                if bal is not None:
                    if last_balance is not None and bal < last_balance:
                        decreases.append({
                            "ts": ev.get("ts"),
                            "slice": f.name,
                            "was": last_balance,
                            "now": bal,
                            "context": list(last_events[-6:]),
                        })
                    last_balance = bal
                    balance_series.append(
                        (ev.get("ts"), f.name, bal))
            elif kind == "fail":
                cur["fails"] += 1
            elif kind == "abort":
                cur["aborts"] += 1
            last_events.append(f"{kind}:{ev.get('amount', ev.get('reason', ''))}")

        slices.append(cur)

    total_c = sum(s["cycles"] for s in slices)
    total_f = sum(s["fails"] for s in slices)
    print(f"slices: {len(slices)} | cycles: {total_c} | fails: {total_f}")
    print("\nper-slice (last 10):")
    for s in slices[-10:]:
        print(f"  {s['file']}: {s['cycles']} cycles, {s['fails']} fails, {s['aborts']} aborts")

    print(f"\nbalance samples: {len(balance_series)}")
    if balance_series:
        print(f"  first: {balance_series[0]}")
        print(f"  last:  {balance_series[-1]}")

    print(f"\nBALANCE DECREASES (reset boundaries): {len(decreases)}")
    for d in decreases:
        print(f"  ts={d['ts']} slice={d['slice']} {d['was']} -> {d['now']}")
        print(f"    context: {d['context']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
