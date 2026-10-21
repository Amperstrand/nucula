"""T4 — nucula presents (`nfc request`) → phone receives → console print.

The flagship device→phone loop: the PN7160 card emulation presents a
Type-4 NDEF tag carrying a NUT-18 payment request; the PHONE initiates
the read (safe direction per the bench ACR/CE doctrine — the only
writer here is the phone paying). Success evidence, machine-parsed:

    NFC payment received: <amount>            (nfc.cpp console_notify)

Coupling probe: `nfc request` runs async with a 120 s firmware
timeout; if the phone is not on the PN7160 antenna nothing couples and
we skip with the geometry reason before the firmware timeout expires.
"""

import re
import time

import pytest

AMOUNT = 12          # sats requested — small, fake, bench mint only
COUPLING_WINDOW_S = 60


def _phone_sees_tag(phone) -> list[str]:
    """Evidence the phone's NFC stack saw + dispatched our tag."""
    return phone.logcat_grep(
        r"NfcService|TagDispatch|NDEF_DISCOVERED|com.cashu.me.*nfc|CashuNfc",
        since_s=COUPLING_WINDOW_S // 2)


def test_nucula_ce_phone_receives_and_pays(nucula_console, phone, evidence_dir):
    con = nucula_console

    # -- clean slate on the device ------------------------------------------
    con.cmd("wipe", timeout_s=30)

    # -- arm the request (async: returns to prompt immediately) -------------
    out = con.cmd(f"nfc request {AMOUNT}", timeout_s=15)
    assert "requesting" in out or "via NFC" in out, out

    # -- stage the phone: awake, wallet foreground (tag dispatch target) ----
    phone.keep_awake()
    phone.launch_cashu()
    phone.save_dump("t4_phone_staged")

    # -- coupling window: watch BOTH sides ------------------------------------
    # Phone side: any NFC dispatch evidence. Device side: the console
    # notify. Whichever comes first proves the antennas couple.
    deadline = time.monotonic() + COUPLING_WINDOW_S
    phone_hits: list[str] = []
    result = None
    while time.monotonic() < deadline:
        phone_hits = _phone_sees_tag(phone)
        con._pump()
        if re.search(r"NFC payment received|NFC token queued", con.buf):
            result = "received"
            break
        if phone_hits:
            # coupled! give the human-visible/app flow time to complete
            result = result or "coupled"
        time.sleep(2)

    con._pump()
    window = con.buf
    con.buf = ""
    (evidence_dir / "t4_console_window.log").write_text(window)
    (evidence_dir / "t4_phone_nfc_logcat.log").write_text(
        "\n".join(phone_hits))

    if result is None:
        pytest.skip(
            "no NFC coupling in "
            f"{COUPLING_WINDOW_S}s (phone not on the PN7160 antenna) — "
            "bench geometry, place the phone on the nucula coil and rerun")

    # coupled: the request was seen. Full payment needs the phone app to
    # complete its send sheet; allow the firmware's own timeout to close.
    con.expect(r"NFC payment received|NFC token queued|payment timeout|nfc error",
               timeout_s=130)
    final = con.buf
    con.buf = ""
    assert "NFC payment received:" in final or "NFC token queued:" in final, (
        f"coupled but no payment print; window:\n{final[-600:]}")
    m = re.search(r"NFC (?:payment received|token queued): (\S+)", final)
    assert m, final[-300:]
