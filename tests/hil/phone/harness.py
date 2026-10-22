"""Shared HIL harness for phone-vs-device cashu wallet testing (nucula).

Doctrine + topology: docs/HIL-PHONE-DESIGN.md. Highlights:

- labgrid coordinates ALL hardware (acquire/release per suite via
  labgrid-client; coordinator ai-legion:20408 by default).
- The nucula-C3's USB-CDC resets the board on host DTR/RTS patterns
  (ccid-firmware-rs #117): consoles open with lines deasserted BEFORE
  open (the /opt/bench/nucula-ccid-broker.py safe-opener) and stay open
  for the whole suite — one reset per suite, asserted as the boot
  banner.
- Phone driving is adb + uiautomator ONLY: the wallet sets FLAG_SECURE,
  screenshots come out black (micronuts PHONE-BRINGUP-RUNBOOK.md);
  web content is invisible to uiautomator — links are tapped by
  coordinates.
- Evidence: every console keeps a transcript, every phone step can dump
  UI XML; all of it lands in the session evidence dir.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import threading
import time
import http.server
from pathlib import Path

import serial

COORDINATOR = os.environ.get("LG_COORDINATOR", "ai-legion:20408")
PHONE_SERIAL = os.environ.get("HIL_PHONE_SERIAL", "RF8NB01ZYAH")

# Console port: resolved from the labgrid place (repo rule — no hardcoded
# device paths), env override for offline dev.
NUCULA_CONSOLE_PORT = os.environ.get("HIL_NUCULA_CONSOLE_PORT", "")
NUCULA_PROMPT = "nucula> "


def resolve_serial_port(place: str = "nucula-c3") -> str:
    """SerialPort resource `port` param from the place (single source of
    truth; falls back to the env default for offline collection)."""
    if NUCULA_CONSOLE_PORT:
        return NUCULA_CONSOLE_PORT
    r = lg(place, "show")
    m = re.search(r"'port':\s*'([^']+)'", r.stdout)
    if not m:
        raise RuntimeError(
            f"no SerialPort 'port' param on place {place}; "
            "set HIL_NUCULA_CONSOLE_PORT explicitly")
    return m.group(1)

BENCH_LINK_PORT = int(os.environ.get("HIL_LINK_PORT", "8077"))
BENCH_LAN_IP = os.environ.get("HIL_BENCH_IP", "192.168.13.208")
DEFAULT_MINT = os.environ.get("MINT_URL", "https://testnut.cashu.space")


# --------------------------------------------------------------------------
# labgrid client
# --------------------------------------------------------------------------

class LabgridBusy(Exception):
    def __init__(self, place: str, holder: str | None, detail: str = ""):
        self.place, self.holder = place, holder
        super().__init__(
            f"labgrid place '{place}' held by {holder or 'unknown session'} {detail}".strip()
        )


def lg(place: str, *args: str, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["labgrid-client", "-x", COORDINATOR, "-p", place, *args],
        capture_output=True, text=True, timeout=timeout, check=False,
    )


def place_holder(place: str) -> str | None:
    """Global `who` table row for a place ('' = free, None = error)."""
    r = lg(place, "who")
    if r.returncode != 0:
        return None
    for line in r.stdout.strip().splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 4 and parts[2] == place:
            return f"{parts[0]}@{parts[1]}"
    return ""


def acquire_place(place: str) -> str:
    """Acquire, tolerating an existing hold by OUR principal (stale-self
    holds from earlier sessions of this lane: the coordinator answers
    'You have already acquired place …', which is success for us)."""
    r = lg(place, "acquire")
    if r.returncode != 0:
        if "already acquired" in r.stderr:
            return place
        raise LabgridBusy(place, place_holder(place), r.stderr.strip())
    return place


def release_place(place: str) -> None:
    lg(place, "release")


class AcquiredPlace:
    """Context manager: acquire on enter, release on exit."""

    def __init__(self, place: str):
        self.place = place

    def __enter__(self) -> "AcquiredPlace":
        acquire_place(self.place)
        return self

    def __exit__(self, *exc) -> None:
        release_place(self.place)


# --------------------------------------------------------------------------
# device console (DTR-safe)
# --------------------------------------------------------------------------

class Console:
    """Line-oriented serial console with prompt sync + pattern expect.

    ONE Console per board per suite: opening the port may reset the
    board (C3 USB-JTAG rst:0x15) — that reset IS the deterministic boot
    we assert on. `boot_banner()` waits for the first prompt.
    """

    def __init__(self, port: str, baud: int = 115200,
                 label: str = "console", evidence_dir: Path | None = None):
        self.label = label
        self.evidence_dir = evidence_dir
        self.transcript_path = (
            evidence_dir / f"{label}.transcript.log"
            if evidence_dir else None
        )
        s = serial.Serial()          # safe opener: DTR/RTS low BEFORE open
        s.port = port
        s.baudrate = baud
        s.timeout = 0.05
        s.write_timeout = 5.0        # a wedged CDC never drains TX
        s.dtr = False
        s.rts = False
        s.open()
        self.ser = s
        self.buf = ""
        self.transcript = ""

    # -- raw plumbing ------------------------------------------------------

    def _pump(self) -> str:
        n = self.ser.inWaiting()
        chunk = self.ser.read(n) if n else b""
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            self.buf += text
            self.transcript += text
        return text if chunk else ""

    def read_until(self, pattern: str, timeout_s: float) -> str:
        """Consume until `pattern` appears in the buffer; return the
        matched window (buffer up to and including the match)."""
        deadline = time.monotonic() + timeout_s
        pat = re.compile(pattern)
        while time.monotonic() < deadline:
            m = pat.search(self.buf)
            if m:
                out, self.buf = self.buf[:m.end()], self.buf[m.end():]
                return out
            self._pump()
            time.sleep(0.02)
        out, self.buf = self.buf, ""
        raise TimeoutError(
            f"[{self.label}] pattern {pattern!r} not seen in {timeout_s}s; "
            f"tail: {out[-400:]!r}"
        )

    # -- nucula console protocol -------------------------------------------

    def boot_banner(self, timeout_s: float = 20.0) -> str:
        return self.read_until(re.escape(NUCULA_PROMPT), timeout_s)

    def cmd(self, line: str, timeout_s: float = 60.0) -> str:
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        return self.read_until(re.escape(NUCULA_PROMPT), timeout_s)

    def expect(self, pattern: str, timeout_s: float) -> str:
        return self.read_until(pattern, timeout_s)

    def close(self) -> None:
        try:
            if self.transcript_path:
                self.transcript_path.write_text(self.transcript)
        finally:
            self.ser.close()


# --------------------------------------------------------------------------
# phone (adb + uiautomator)
# --------------------------------------------------------------------------

class Phone:
    """cashu.me driving over adb. uiautomator dump + text-bounds taps
    (FLAG_SECURE kills screenshots); web links are tapped by bounds."""

    def __init__(self, serial_: str = PHONE_SERIAL, evidence_dir: Path | None = None):
        self.serial = serial_
        self.evidence_dir = evidence_dir

    def sh(self, cmd: str, timeout: int = 30) -> str:
        r = subprocess.run(
            ["adb", "-s", self.serial, "shell", cmd],
            capture_output=True, text=True, timeout=timeout, check=False)
        if r.returncode != 0:
            raise RuntimeError(f"adb shell {cmd!r} rc={r.returncode}: {r.stderr.strip()}")
        return r.stdout

    def dump_xml(self) -> str:
        """One uiautomator dump; '' on failure (concurrent uiautomator
        sessions kill each other with rc=137 — a shared-phone lane is a
        SKIP reason, not a suite error)."""
        for _ in range(2):
            r = subprocess.run(
                ["adb", "-s", self.serial, "shell",
                 "uiautomator dump /sdcard/hil_ui.xml"],
                capture_output=True, text=True, timeout=60, check=False)
            if r.returncode == 0:
                time.sleep(0.5)
                return self.sh("cat /sdcard/hil_ui.xml")
            time.sleep(2)
        return ""

    def screen_texts(self) -> list[str]:
        return re.findall(r'text="([^"]{2,60})"', self.dump_xml())

    def tap_text(self, needle: str, settle: float = 2.0) -> bool:
        xml = self.dump_xml()
        pat = (r'<node[^>]*text="[^"]*' + re.escape(needle) +
               r'[^"]*"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')
        m = re.search(pat, xml)
        if not m:
            return False
        x = (int(m.group(1)) + int(m.group(3))) // 2
        y = (int(m.group(2)) + int(m.group(4))) // 2
        self.sh(f"input tap {x} {y}")
        time.sleep(settle)
        return True

    def tap_bounds(self, x: int, y: int, settle: float = 2.0) -> None:
        self.sh(f"input tap {x} {y}")
        time.sleep(settle)

    def keep_awake(self) -> None:
        self.sh("svc power stayon true")
        self.sh("settings put system screen_off_timeout 7200000")
        self.sh("input keyevent KEYCODE_WAKEUP")
        # swipe up past the (unlock-less) lockscreen if one is showing
        self.sh("input swipe 540 1800 540 400 200")
        time.sleep(1.0)

    def foreground_app(self) -> str:
        out = self.sh("dumpsys window | grep mCurrentFocus")
        m = re.search(r"u0 ([^/]+)/([^ }]+)", out)
        return m.group(1) if m else out.strip()

    def launch_cashu(self) -> None:
        # Samsung app-sleeping DISABLES unused apps (observed 2026-10-21:
        # enabled=3 after four idle days) — re-enable before launching.
        self.sh("pm enable --user 0 com.cashu.me")
        self.sh("monkey -p com.cashu.me -c android.intent.category.LAUNCHER 1")
        time.sleep(4.0)

    def logcat_grep(self, pattern: str, since_s: int = 30) -> list[str]:
        out = self.sh(
            f"logcat -t {since_s * 20} -v brief 2>/dev/null | grep -i -E '{pattern}' | tail -20"
        )
        return [l for l in out.splitlines() if l.strip()]

    def save_dump(self, name: str) -> Path | None:
        if not self.evidence_dir:
            return None
        xml = self.dump_xml()   # best-effort: contention returns ''
        if not xml:
            return None
        p = self.evidence_dir / f"{name}.ui.xml"
        p.write_text(xml)
        return p

    # -- token receive over the browser-link channel ------------------------

    def open_link_page_and_tap(self, host: str, port: int) -> None:
        """The proven token hand-off: open the bench link page in the
        browser and tap the link BY COORDINATES (uiautomator cannot see
        web content). Handles: the no-default-browser resolver (pick
        Samsung Browser/Chrome + Just once — bench phone has no default),
        Samsung Internet first-run onboarding, then the link tap (page
        layout is deterministic: single <a>, padding 40px, font 40px →
        center ~ (270, ~110))."""
        # Launch Chrome BY COMPONENT: the browser role on this bench
        # phone is contested (Samsung Browser nags for default and can
        # win the intent); the component launch bypasses both the
        # resolver and the nag entirely.
        self.sh("am force-stop com.sec.android.app.sbrowser")
        self.sh("am start -n com.android.chrome/"
                "com.google.android.apps.chrome.IntentDispatcher "
                f"-d http://{host}:{port}/")
        time.sleep(5.0)
        # browser resolver ("Complete action using") — no default is set
        if self.tap_text("Samsung Browser", settle=3.0) or \
                self.tap_text("Chrome", settle=3.0):
            self.tap_text("Just once", settle=4.0)
        # Samsung Internet first-run onboarding (Continue/Next/Start)
        for _ in range(3):
            if not self.tap_text("Continue", settle=2.0):
                break
        for _ in range(3):
            if not self.tap_text("Next", settle=2.0):
                break
        self.tap_text("Start", settle=2.0)
        time.sleep(3.0)
        # tap the link by coordinates — center of the full-screen block
        self.tap_bounds(540 // 2, 1200, settle=6.0)
        # cashu: deep-link resolver (bench phone has 2 wallets installed:
        # cashu.me "Cashu" + "Minibits") — pin to Cashu, ALWAYS (bench
        # determinism; this is a dedicated test phone).
        if self.tap_text("Cashu", settle=4.0):
            if not self.tap_text("Always", settle=4.0):
                self.tap_text("Just once", settle=4.0)


# --------------------------------------------------------------------------
# browser-link token server
# --------------------------------------------------------------------------

class TokenLinkServer:
    """Serves `<a href="cashu:<token>">TAP TO RECEIVE TOKEN</a>` on the
    bench LAN (ufw 8077/tcp is already open). Page layout matches the
    proven tokenserve template so the phone taps a stable coordinate."""

    # Full-screen tap target: uiautomator cannot see web content, so the
    # phone taps by coordinates — the link block fills the viewport and
    # any tap below the browser chrome lands on it.
    PAGE = (
        "<html><head><meta name='viewport' content='width=device-width, "
        "initial-scale=1'></head><body style=\"background:#111;color:#fff;"
        "margin:0\"><a id=\"t\" style=\"display:block;color:#6cf;"
        "font-size:48px;text-align:center;padding:220px 30px\" "
        "href=\"cashu:{token}\">TAP TO RECEIVE TOKEN</a></body></html>"
    )

    def __init__(self, workdir: Path, port: int = BENCH_LINK_PORT):
        self.port = port
        (workdir / "index.html").write_text(self.PAGE.format(token="pending"))
        self.root = str(workdir)
        handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(
            *a, directory=self.root, **kw)
        self.httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def serve_token(self, token: str) -> None:
        (Path(self.root) / "index.html").write_text(
            self.PAGE.format(token=token))

    def stop(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


# --------------------------------------------------------------------------
# rig payer (host-side token minting)
# --------------------------------------------------------------------------

def mint_token_via_rig(repo_root: Path, mint_url: str, sats: int = 21,
                       timeout_s: int = 300) -> str:
    """Run the rig payer ignored-test and scrape `TOKEN=cashuB…`.

    The rig is the single source of minting truth on the bench
    (cdk-based, MINT_URL env, auto-settling fake-sat quotes). Uses a
    per-checkout CARGO_TARGET_DIR: the host shares one global target
    dir (~/.cargo/config.toml) and concurrent lanes collide there.
    """
    env = dict(os.environ,
               MINT_URL=mint_url,
               HIL_MINT_SATS=str(sats),
               CARGO_TARGET_DIR=str(repo_root / "rig" / "target-hil"))
    r = subprocess.run(
        ["cargo", "test", "--features", "payer", "--manifest-path",
         str(repo_root / "rig" / "Cargo.toml"),
         "mints_a_token_from_the_running_mint",
         "--", "--ignored", "--nocapture"],
        capture_output=True, text=True, timeout=timeout_s, check=False, env=env)
    m = re.search(r"TOKEN=(cashu[AB][A-Za-z0-9_\-]+)", r.stdout + r.stderr)
    if not m:
        raise RuntimeError(
            f"rig payer produced no token (rc={r.returncode}); "
            f"tail: {(r.stdout + r.stderr)[-600:]!r}")
    return m.group(1)


def esptool(*args: str, timeout: int = 600) -> subprocess.CompletedProcess:
    return subprocess.run(["esptool", *args], capture_output=True,
                          text=True, timeout=timeout, check=False)
