"""T3 — phone presents (HCE Send-by-NFC) → device-side reader.

Money-proof path today: the ACR1252 reference reader reads the phone's
Type-4 HCE tag with SELECT + READ_BINARY sequences ONLY (bench wedge
rule: CE-data WRITES through the ACR are forbidden; reads are safe),
then the harness proves the payload is a spendable cashu token by
shape. The on-device MFRC522 reader boards (nucula fork m5stick/atom,
micronuts esp32-wallet T5) cover the phone-HCE *detection* half at
ISO-14443-3 level.

Coupling probe: if the phone is not on the reader antenna, the SELECT
fails → loud skip with the geometry reason (never fake a tap).
"""

import re
import time

import pytest

from harness import lg

NDEF_AID = "D2760000850101"   # cashu.me HCE NDEF application (mission fact)

SELECT_AID = f"00A40400 07 {NDEF_AID}"
SELECT_CC = "00A4000C 02 E103"
SELECT_NDEF_FILE = "00A4000C 02 1041"


def _acr_picc_name() -> str | None:
    from smartcard.System import readers
    rs = [str(r) for r in readers()]
    cands = [r for r in rs if "ACR1252" in r and "PICC" in r]
    return cands[0] if cands else None


def _transmit(conn, apdu_hex: str):
    from smartcard.util import toBytes
    data, sw1, sw2 = conn.transmit(toBytes(apdu_hex.replace(" ", "")))
    return bytes(data), (sw1 << 8) | sw2


def _read_t4_ndef(conn) -> bytes:
    """Minimal Type-4 NDEF read: SELECT NDEF app → SELECT CC → read CC
    → SELECT NDEF file → READ_BINARY its length + payload. READS ONLY."""
    _, sw = _transmit(conn, SELECT_AID)
    if sw != 0x9000:
        raise ConnectionError(f"NDEF app select SW={sw:04X} (no HCE tag in field?)")
    cc, sw = _transmit(conn, SELECT_CC)
    assert sw == 0x9000, f"CC select SW={sw:04X}"
    # CC byte 11..12 = NDEF file length (little endian) in mapping v1.0
    flen = int.from_bytes(cc[11:13] if len(cc) >= 13 else b"\x00\x00", "big")
    _, sw = _transmit(conn, SELECT_NDEF_FILE)
    assert sw == 0x9000, f"NDEF file select SW={sw:04X}"
    hdr, sw = _transmit(conn, "00B00000 02")
    assert sw == 0x9000
    n = int.from_bytes(hdr[:2], "big")
    payload, sw = _transmit(conn, f"00B00002 {min(n, 0xFF):02X}")
    while len(payload) < n:   # long tokens: keep reading (still reads)
        more, sw = _transmit(conn, f"00B0{2 + len(payload):04X} "
                           f"{min(n - len(payload), 0xFF):02X}")
        if sw != 0x9000:
            break
        payload += more
    return payload[:n] if n else payload


def _stage_phone_hce_send(phone) -> bool:
    """Drive cashu.me into the NFC-send state (Send → … NFC …).

    App UI drifts; this is deliberately a best-effort stager: if the
    flow is not findable in the dump, we skip with what we saw. """
    phone.launch_cashu()
    if not phone.tap_text("Send", settle=3.0):
        return False
    if phone.tap_text("NFC", settle=3.0):
        return True
    # some builds put it behind "Send to" / amount entry first
    phone.tap_text("amount", settle=1.0)
    return phone.tap_text("NFC", settle=3.0)


def test_acr_reads_phone_hce_token(acr_place, phone, evidence_dir):
    if _acr_picc_name() is None:
        pytest.skip("ACR1252 PICC not present in pcscd")

    staged = _stage_phone_hce_send(phone)
    texts = phone.screen_texts()
    phone.save_dump("t3_phone_send_staged")
    if not staged:
        pytest.skip(
            "cashu.me NFC-send flow not stageable via uiautomator "
            f"(screen texts: {texts[:8]})")

    from smartcard.System import readers
    rs = [r for r in readers() if "ACR1252" in str(r) and "PICC" in str(r)]
    conn = rs[0].createConnection()
    try:
        conn.connect()
    except Exception as e:
        pytest.skip(f"no tag coupled to ACR antenna ({e}) — phone placement "
                    "is bench geometry; place phone on the ACR and rerun")

    payload = b""
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        try:
            payload = _read_t4_ndef(conn)
            break
        except ConnectionError:
            time.sleep(2)
    (evidence_dir / "t3_ndef_payload.bin").write_bytes(payload or b"<empty>")
    assert payload, "HCE tag answered but carried no NDEF payload"

    text = payload.decode("utf-8", errors="replace")
    assert re.search(r"cashu[AB]", text), f"not a cashu token NDEF: {text[:80]!r}"
