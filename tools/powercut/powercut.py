#!/usr/bin/env python3
"""Power-cut loss-window quantifier for nucula-on-tollgate (research).

Sweeps DTR-reset cuts across the receive path (token POST -> swap ->
adopt -> NVS save) and measures, per cut, whether funds were lost:

  outcome per offset:
    clean-abort   mint says UNSPENT, wallet unchanged  (no loss)
    recovered     mint SPENT, wallet +amount           (no loss)
    LOST          mint SPENT, wallet unchanged         (the window)
    torn          wallet boots DEGRADED (blob torn)    (its own class)

Phase attribution is exact per cut: the device console timestamps
(POST received / swap response / Payment OK) classify where the cut
landed, independent of adb-start jitter.

Bench: ai-legion, t7670-wanderer (serial by-id) + moto g (adb).
Mint: our fakewallet (cdk 0.17.2, replay-capable). Tokens: token-minter.
checkstate: direct to the mint from the bench (Ys = hash_to_curve(secret)).

Usage: powercut.py [--offsets 100:1900:100] [--reps 2] [--calibrate]
Results: CSV to stdout + results/POWERCUT-*.md rows.
"""
import argparse
import base64
import json
import os
import re
import serial
import subprocess
import sys
import threading
import time
import urllib.request

PORT = "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5C8D049416-if00"
ADB = ["adb", "-s", "ZY326DPC7R", "shell"]
ADB_ENV = dict(os.environ, ADB_SERVER_SOCKET="tcp:127.0.0.1:5037")
MINT = os.environ.get("MINT", "https://nodns.shop/mint")
GW = "172.29.139.1"
SSID = "TollGate-PoC"

# ---- cashu-audit must-gap-vectors point ops (nutshell secp.py mirror) ----
import hashlib
from coincurve import PublicKey

DST = b"Secp256k1_HashToCurve_Cashu_"

def sha256(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()

def hash_to_curve(msg: str) -> bytes:
    h = sha256(DST + msg.encode())
    for ctr in range(2**16):
        try:
            return PublicKey(b"\x02" + sha256(h + ctr.to_bytes(4, "little"))).format()
        except Exception:
            continue
    raise ValueError("no point found")


def token_secrets(tok: str):
    raw = base64.urlsafe_b64decode(tok[6:] + "==")
    d = json.loads(raw)
    tk = d.get("token", [{}])[0] if isinstance(d.get("token"), list) else d
    return [p["secret"] for p in tk["proofs"]], sum(p["amount"] for p in tk["proofs"])


def checkstate(secrets):
    ys = [hash_to_curve(s).hex() for s in secrets]
    body = json.dumps({"Ys": ys}).encode()
    req = urllib.request.Request(MINT + "/v1/checkstate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as r:
        d = json.load(r)
    states = [s["state"] for s in d["states"]]
    if all(s == "UNSPENT" for s in states):
        return "UNSPENT"
    if all(s == "SPENT" for s in states):
        return "SPENT"
    return "MIXED:" + ",".join(states)


def mint_token():
    out = subprocess.run(
        ["cargo", "run", "-q", "-p", "token-minter"],
        cwd="/root/src/tollgate-s3-rs",
        env=dict(os.environ, MINT=MINT, PATH="/root/.cargo/bin:" + os.environ["PATH"]),
        capture_output=True, text=True, timeout=180)
    for line in out.stdout.splitlines():
        if line.startswith("cashuA"):
            return line.strip()
    raise RuntimeError(f"minter: {out.stdout[-200:]} {out.stderr[-200:]}")


class Console:
    def __init__(self):
        self.ser = serial.Serial(PORT, 115200, timeout=0.2)
        self.buf = []
        self.alive = True
        self.th = threading.Thread(target=self._read, daemon=True)
        self.th.start()

    def _read(self):
        while self.alive:
            try:
                data = self.ser.read(4096).decode("utf-8", "replace")
                if data:
                    self.buf.append(data)
            except Exception:
                time.sleep(0.2)

    def text(self):
        return "".join(self.buf)

    def cut(self):
        """Power-cut emulation: assert EN (via DTR/RTS auto-reset circuit)
        long enough to drop the board mid-operation, then release."""
        try:
            self.ser.dtr = True
            self.ser.rts = True
            time.sleep(0.15)
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass

    def close(self):
        self.alive = False
        time.sleep(0.3)
        try:
            self.ser.close()
        except Exception:
            pass


def sh(cmd, timeout=60):
    return subprocess.run(ADB + [cmd] if isinstance(cmd, str) else cmd,
                          env=ADB_ENV, capture_output=True, text=True, timeout=timeout).stdout


def ensure_phone():
    for attempt in range(3):
        if "172.29.139." in sh("ip addr show wlan0"):
            return
        if attempt == 0:
            sh(f"cmd wifi connect-network {SSID} open")
        else:
            # full radio cycle: the phone's stack flakes after many cycles
            sh("svc wifi disable")
            time.sleep(4)
            sh("svc wifi enable")
            time.sleep(8)
            sh(f"cmd wifi connect-network {SSID} open")
        for _ in range(20):
            time.sleep(2)
            if "172.29.139." in sh("ip addr show wlan0"):
                return
    raise RuntimeError("phone did not join the AP")


def wait_boot(con, settle=18):
    t0 = time.time()
    saw_boot = False
    while time.time() - t0 < 60:
        t = con.text()
        if not saw_boot and ("boot:ESP-IDF" in t or "Compile time" in t):
            saw_boot = True
        m = re.findall(r"Wallet ready: balance=(\d+)", t)
        deg = "wallet is DEGRADED" in t
        if m or deg:
            time.sleep(settle)   # let it finish boot logs
            return (int(m[-1]) if m else None), deg
        time.sleep(1)
    return None, saw_boot


def run_cut(con, offset_ms):
    tok = mint_token()
    secrets, amount = token_secrets(tok)
    ensure_phone()
    con.buf.clear()
    t0 = time.time()
    p = subprocess.Popen(ADB + [f"curl -s -m 40 -d 'token={tok}' http://{GW}/pay"],
                         env=ADB_ENV, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(offset_ms / 1000.0)
    pre = con.text()          # phase evidence lives here — snapshot it
    con.cut()
    p.wait(timeout=30)
    con.buf.clear()
    time.sleep(3)
    bal, degraded = wait_boot(con)
    if bal is None:
        # issue #24: some cuts hard-hang the board with no WDT recovery —
        # pulse a second reset so the sweep can continue; mark the row
        con.cut()
        con.buf.clear()
        time.sleep(3)
        bal, degraded = wait_boot(con)
    state = checkstate(secrets)
    log = pre + "\n===CUT===\n" + con.text()
    phase = "unknown"
    if "Payment OK" in log:
        phase = "post-commit"
    elif re.search(r"swap: \d+ inputs ->", log):
        phase = "post-swap-response"
    elif "POST https://" in log and "swap" in log:
        phase = "swap-in-flight"
    elif "POST /pay received" in log:
        phase = "pre-swap"
    else:
        phase = "pre-post"
    return {
        "offset_ms": offset_ms, "phase": phase, "amount": amount,
        "mint_state": state, "boot_balance": bal, "degraded": degraded,
        "hang": bal is None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offsets", default="100:1900:100")
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--calibrate", action="store_true")
    args = ap.parse_args()

    a, b, s = (int(x) for x in args.offsets.split(":"))
    offsets = list(range(a, b + 1, s))

    con = Console()
    print("offset_ms,rep,phase,amount,mint_state,boot_balance,degraded,hang",
          flush=True)
    try:
        # baseline: current balance with no cut
        con.buf.clear()
        bal0, deg0 = wait_boot(con, settle=4)
        if args.calibrate:
            for i in range(3):
                r = run_cut(con, 10_000)   # no effective cut (payment done)
                print(f"cal,{i},{r['phase']},{r['amount']},{r['mint_state']},"
                      f"{r['boot_balance']},{r['degraded']}", flush=True)
        prev_bal = bal0
        for off in offsets:
            for rep in range(args.reps):
                r = run_cut(con, off)
                lost = (r["mint_state"] == "SPENT"
                        and r["boot_balance"] is not None
                        and r["boot_balance"] - (prev_bal or 0) < r["amount"])
                print(f"{off},{rep},{r['phase']},{r['amount']},"
                      f"{r['mint_state']},{r['boot_balance']},{r['degraded']},"
                      f"{r['hang']}", flush=True)
                if r["boot_balance"] is not None:
                    prev_bal = r["boot_balance"]
                time.sleep(2)
    finally:
        con.close()


if __name__ == "__main__":
    main()
