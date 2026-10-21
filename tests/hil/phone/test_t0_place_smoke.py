"""T0 — place smoke: the bench is alive and excludable.

Proves: coordinator reachable, each place acquires+releases, the phone
enumerates over adb, and the nucula console resets+boots to a prompt
(one open per suite — harness.Console). Runs against WHATEVER firmware
the board currently serves: the banner assert is
`nucula> `-prompt-shaped, so a board on another project's image skips
loudly instead of pretending.
"""

import re

import pytest

from harness import Console, NUCULA_CONSOLE_PORT


def test_coordinator_places_listed():
    from harness import lg
    r = lg("phone-s20", "show")
    assert r.returncode == 0, r.stderr
    assert "USBDevice" in r.stdout


def test_place_acquire_release_cycle(request):
    if not request.config.getoption("--hil"):
        pytest.skip("hardware gated: pass --hil on the bench runner")
    from harness import AcquiredPlace, place_holder
    with AcquiredPlace("phone-s20"):
        assert place_holder("phone-s20") is not None
    assert place_holder("phone-s20") == ""


def test_phone_adb_transport(phone):
    out = phone.sh("getprop ro.build.version.release")
    assert out.strip().startswith("1"), f"unexpected Android: {out!r}"
    pkg = phone.sh("pm list packages com.cashu.me")
    assert "com.cashu.me" in pkg, "cashu.me wallet not installed"
    hce = phone.sh(
        "dumpsys package com.cashu.me | grep -c CashuNfcHostApduService")
    assert int(hce.strip() or 0) > 0, "cashu.me HCE service missing"


def test_nucula_console_banner(nucula_console, evidence_dir):
    """Opening the console reset the board; the boot must land on the
    nucula> prompt (wallet firmware present + console alive)."""
    banner = (evidence_dir / "nucula.banner.log").read_text()
    assert "nucula> " in banner
    # the reset signature is honest evidence, not noise
    assert re.search(r"rst:0x|boot:|nucula", banner, re.I), banner[-200:]
