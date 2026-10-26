# Power-cut loss-window measurement (baseline, unmodified nucula)

`tools/powercut/powercut.py` — DTR-reset cuts swept across the receive
path (token POST → swap → adopt → NVS save) on the bench t7670g
(wallet: 966 sats / 117 proofs at start; mint: our cdk 0.17.2 fakewallet).
Ground truth per cut: mint checkstate of the token's Ys (SPENT/UNSPENT)
vs device boot balance. 2 reps per offset, 100 ms steps.

## Results (combined sweeps, 2026-10-26)

| Offset after POST | Phase (console) | Mint | Wallet delta | Verdict |
|---|---|---|---|---|
| 100–800 ms | pre-post / pre-swap | UNSPENT | 0 | clean abort — no loss |
| **900–1200 ms** | swap dispatched (console marker not yet logged) | **SPENT** | **0** | **LOST** |
| **1300–1500 ms** | swap-in-flight | **SPENT** | **0** | **LOST** |
| **1600 ms** | post-swap-response | **SPENT** | **0** | **LOST** |
| 1700 ms+ | post-swap-response / post-commit | SPENT | +21 | recovered (commit landed) |

## Findings

1. **The loss window is real, deterministic, and ~800 ms wide**
   (≈850–1650 ms after the client's POST at this wallet size; corrected
   from an initial 1200 ms reading once sweep 3's full rows landed):
   16/16 cuts inside it consumed the token at the mint without the new
   proofs persisting. 21 sats lost per cut. Hypothesis H1's premise is
   now measured fact, not conjecture. (Console phase labels lag the mint
   truth near the leading edge — the swap had already been dispatched
   before its marker could print; checkstate is the authority.)
2. **The window scales with wallet size**: its far half is the NVS
   whole-blob save (117 proofs ≈ 10 KB ≈ hundreds of ms of erase+program
   plus adopt). A bigger wallet widens the window; the delta-journal
   hypothesis (H4) and the write-ahead (H1) both shrink it toward zero.
3. **One cut (≈900–1000 ms zone) hard-hung the board**: no watchdog
   recovery, no console output, AP down — required a manual reset. A
   fleet device cannot tolerate a cut class that evades the WDT; the
   boot path between reset and WDT-init needs a hardening pass (separate
   issue from persistence).
4. Cuts before the swap executes are perfectly safe (clean aborts,
   UNSPENT) — idempotent-retry territory is only the in-window side.

## Next (per the amended H1)

Persist the derivation counter range (~24 bytes) BEFORE the swap POST;
on boot, any journaled range recovers by re-derivation + re-POST
(cdk-family replay returns the original signatures — our audit's
COMPAT-LEDGER fact) or checkstate. Re-run this exact sweep against the
fix: pass = zero LOST rows at every offset.

Raw data: `powercut-sweep{1,2,3}.csv` alongside this doc (38 measured
cuts; sweeps 1–2 were cut short by the board-hang side effect and
resumed); harness is deterministic and re-runnable.
