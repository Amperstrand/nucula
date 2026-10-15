#!/bin/bash
# LAN mint for the nucula rig — adapter fronts mint_server (FakeWallet, auto-settling).
# Recreated 2026-10-15 (the /tmp/opencode/rig/ original was ephemeral). Durable: lives in the fork worktree.
export MICRONUTS_MINT_BIN=/root/.cargo-target/debug/mint_server
export MICRONUTS_ADAPTER_PORT=3338
export MICRONUTS_ADAPTER_BIND=0.0.0.0
exec /root/.cargo-target/debug/micronuts-audit-adapter
