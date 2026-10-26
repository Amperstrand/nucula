# Money safety on a microcontroller

Saga patterns, persistence, and recovery for a constrained Cashu wallet —
research, bench evidence, and hypotheses. **Status: research document,
not a merge proposal.** Nothing here is implemented in nucula; each
hypothesis names the experiment that would justify (or kill) it.

Sources: bench work on the t-a7670g tollgate (Amperstrand/t-a7670g-tollgate
issues #23 closed, and issue #43 here), a source survey of cdk 0.18.1
(`saga_recovery.rs`, `start_up_check.rs`, `cdk-common/src/mint.rs`,
`wallet/recovery.rs`, `cdk-common/src/wallet/saga/`), the nutshell 0.17.3
core crate, the tollgate Go reference, and wire-measured facts from the
cashu-audit compat ledger (`research/COMPAT-LEDGER.md`).

---

## 1. What the bench taught us about MCU constraints

Each failure below was reproduced, root-caused with instrumentation, and
fixed on the bench (t-a7670g-tollgate #23). The lessons are the
constraint ledger any wallet design must respect on this class of device:

1. **Internal RAM is the scarce resource and it is shared with the radio
   stack.** ~520 KB total; after WiFi/lwIP/RTOS, ~150–250 KB usable. At
   800+ wallet-sats (~117 proofs ≈ hundreds of small heap strings), the
   wallet starved httpd accept buffers and mbedtls' 16 KB handshake
   allocation — surfacing as bogus TLS "fatal alerts" and a dead portal.
   Fixes: external (PSRAM) mbedtls allocator; PSRAM allocation threshold
   lowered. Structural lesson: **the wallet's internal-RAM footprint must
   be flat, not growing with proof count.**
2. **Flash writes stall the other core** (XIP): a long NVS erase+program
   window under load tripped the 800 ms interrupt watchdog (issue #43's
   crash class, wallet-size-correlated). Lesson: **bound every commit to
   a few KB; the persistence format must not grow per-save cost with
   wallet size** (whole-blob rewrites violate this).
3. **The upstream network lies.** CGNAT path silently dropped external
   UDP 53; a "dead resolver" turned into a total name-resolution wedge
   via lwIP's 4-slot DNS table. Lesson: recovery paths must assume the
   mint is reachable only sometimes; retries with backoff; never hold
   unbounded pending state.
4. **Crashes and reboots are a operating condition, not an exception.**
   Watchdog, brownout, scheduled maintenance — the wallet must treat
   every reboot as a checkpoint to reconcile from, not a disaster.

## 2. What the ecosystem already learned

### cdk 0.18.x — the full doctrine (server-grade, but the semantics are portable)

The mint persists a **saga record** per in-flight operation:
`Saga { operation_id (correlation key), operation_kind, state, quote_id,
finalization_data }` (`cdk-common/src/mint.rs`). Two rules to copy
verbatim, from its own doc comments:

- **Write intent before external effects.** The melt saga records
  `PaymentAttempted` *before dispatch to the payment backend*, because
  "a terminal negative status cannot prove that the payment was not
  sent."
- **Persist failure before compensating.** `PaymentFailed` is persisted
  "before compensation so a crash cannot turn it back into an ambiguous
  payment attempt."
- **Recovery classifies, never re-attempts blindly.** On boot,
  `start_up_check.rs` walks incomplete sagas: *Compensate* only when no
  external call was possible; otherwise *check external state* (bounded
  by a 10 s timeout) and finalize-or-rollback. "Polling is
  finalization-only" for non-terminal states — never fire a second
  payment attempt, only settle the first.

The cdk **wallet** mirrors the pattern (`wallet/recovery.rs`: "follows
the same pattern as the mint's start_up_check"): receive states are
`ProofsPending → SwapRequested`; reserved proofs are un-stuck at boot by
asking the mint (NUT-07 checkstate) what actually happened. Storage is
SQLite in WAL mode — write-ahead at the storage layer as well.

### nutshell — the opposite pole: stateless core + recovery primitives

The 0.17.3 core crate keeps **no state at all**: tokens/proofs are plain
data and state management belongs to the embedding application. Its
contributions to this problem are the two primitives any MCU wallet can
reuse:

- **NUT-07 checkstate** — the mint is the source of truth for
  spent-ness; any local state can be reconciled against it.
- **Deterministic secrets (nut-06 style)** — secrets derived from a seed
  (not random), so a destroyed local store can be reconstructed:
  re-derive candidates, ask the mint which are spent, keep the rest.
  This is "deterministic state so we can always recover," literally.

### tollgate (Go reference) — minimal state, compensation at the protocol layer

Config/identities as JSON files; sessions purely in RAM. Its
session-ticket/rebind endpoints exist precisely because session state is
not durable — tickets *are* its crash/roaming compensation. No wallet
saga in the basic module.

### cashu-audit measured facts that gate any MCU recovery design

- **Replay caching diverges by family** (COMPAT-LEDGER): byte-identical
  swap re-POST → `200 cached` with the *same signatures* on cdk/cashu-cf
  (cache keyed on inputs); `400` on nutshell mints. Therefore
  **retry-with-same-outputs is a crash-recovery primitive only on
  cdk-family mints.** Our mint (cdk 0.17.2) has the good behavior; the
  dependency must be documented, not assumed.
- **Mint-side quote persistence is durable** (parked quotes mintable
  across upgrade ladders) and NUT-04 quotes are always locked — the
  mint's quote table is a reliable recovery anchor for claim-after-crash.
- cdk's replay cache is keyed on **inputs**: a retry that re-randomizes
  outputs gets "inputs already spent" — recovery must persist and reuse
  the original blinded outputs.

## 3. Constraint ledger for this device class

| Resource | Budget | Consequence |
|---|---|---|
| Internal SRAM (usable) | ~150–250 KB, shared with WiFi/lwIP | Wallet footprint must be O(1) in proof count |
| PSRAM | 4 MB, data-only, slower, no DMA | Proofs, keysets, TLS buffers belong here |
| NVS partition | ~256 KB, 4 KB sectors, ~100k erase cycles | ~25 GB lifetime write budget (wear-leveled) |
| Interrupt WDT | 800 ms (CPU1) | Any flash-op sequence must be split/bounded |
| Mint reachability | Intermittent by assumption | Every external step resumable, never duplicated |

Flash-wear math, so it stops being a mystery: at 100 payments/day with
today's whole-blob 10 KB save, the wear budget is ~70 years; a delta
journal (hundreds of bytes/payment) pushes it past a century. **Wear is
a non-issue at bounded blob size — unbounded growth and write *latency*
are the real hazards** (per-save cost rises until the blob exceeds NVS's
~32 KB entry cap, and long writes stall XIP).

## 4. Hypotheses (each with its killing experiment)

- **H1 — Premint journal (write-ahead before swap).** Persist blinding
  factors + secrets + blinded outputs *before* the swap POST. Crash
  between swap and save becomes recoverable: re-POST the same swap; on a
  cdk-family mint the replay cache returns the original signatures;
  unblind from the journaled premint. *Experiment: bench harness that
  power-cuts (DTR reset) at the window between swap-response and NVS
  commit; assert funds recover on boot. Pass = zero permanent loss across
  ≥20 cuts. Cost: ~1–2 KB NVS per in-flight receive, cleared on commit.*
- **H2 — Quote-id journal before invoice return.** Persist active
  ln-quote ids before the client sees the invoice; on boot re-check
  each, mint+grant the PAID ones (mint quotes are locked and single-use,
  so the retry is safe by NUT-04). *Experiment: kill the device after
  invoice return, settle the invoice, reboot; assert the quote is
  claimed and granted, not orphaned.*
- **H3 — Two-slot A/B blob.** Write the inactive slot, flip an atomic
  marker, erase the old. A torn write can never destroy the only good
  copy. *Experiment: fuzz power-cut during blob write (the #15-era
  fail-stop tests extend naturally); assert boot always finds one valid
  slot.*
- **H4 — Delta journal + compaction.** Append per-receive deltas;
  rewrite the full blob only when the journal exceeds a threshold.
  Bounds per-commit bytes (XIP-stall rule) independent of wallet size.
  *Experiment: measure max commit latency vs wallet size before/after;
  assert flat under the watchdog budget.*
- **H5 — Seed-derived secrets + checkstate reconstruction** (nutshell
  nut-06 style). Secrets derived from a device seed + counter; a
  high-water counter is the only critical durable fact. Wallet
  reconstruction = derive candidates, batch checkstate, keep unspent.
  *Experiment: wipe NVS entirely, boot, assert wallet balance
  reconstructs to the mint's truth within N checkstate round-trips.*
- **H6 — Bounded wallet via consolidation swap.** Cap proof count (e.g.
  48–64); when a receive would exceed it, swap inputs to a
  minimal-denomination output set. Makes H4's blob size and the
  internal-RAM ceiling structural rather than luck. *Experiment: soak
  across the cap boundary; assert footprint (int heap, blob bytes) is
  flat for ±50 sats around the cap.*

## 5. Deliberately not ported

- cdk's generic saga framework and DB layer (SQLite/WAL needs a
  filesystem and ~300 KB — wrong weight class).
- Multi-mint support (single-mint is a correctness simplification the
  tollgate profile already enforces via `unsupported mint_url`).
- Nutshell-family-mint recovery paths without replay caching (out of
  scope until a deployment targets one; documented as a dependency).

## 6. Open questions

- Recovery on nutshell-family mints without replay cache: the swap
  response itself must be journaled before ack (smaller crash window,
  still nonzero). Worth a divergence probe series before relying on it.
- At-rest custody: NVS plaintext proofs are extractable; eFuse-burned
  AES key encrypting the blob closes it — fleet decision, not a bench
  one.
- Whether the 800 ms WDT should be tuned up for flash-heavy boots vs
  splitting commits — H4 answers this empirically.

## 7. Experiment home

Bench: ai-legion, t-a7670-wanderer + moto g rig (e2e harness in
Amperstrand/t-a7670g-tollgate; soak scripts replicate full-corpus runs
with wallet retention). Power-cut injection: DTR reset from the harness
(the rig already reopens the console port; a mid-write reset is a
one-line extension).
