#!/usr/bin/env python3
"""CONFIRMED reproducer for the ACR1252U CE-write wedge (2026-09-07).

Phase B's 6x48-byte chunked writes reproduce the firmware bug: writes
1-3 apply fully (~142 ms each), write 4 partially applies (length echo
36/48, SW still 9000), write 5 wedges the CCID loop until physical
replug. Kept as the regression demo for AGENTS.md "The ACR1252U
wedge"; phase A replicates the rig preflight. Do NOT run it on a
power cycle you need for a real presentation — it burns the reader.
"""
import sys, time

from smartcard.System import readers
from smartcard.CardConnection import CardConnection
from smartcard.scard import SCARD_CTL_CODE, SCARD_SHARE_DIRECT

ESC = SCARD_CTL_CODE(1)


def connect():
    rs = list(readers())
    idx = [i for i, r in enumerate(rs) if "PICC" in str(r)][0]
    c = rs[idx].createConnection()
    c.connect(CardConnection.T0_protocol, mode=SCARD_SHARE_DIRECT)
    return c


def ctl(c, label, bytes_, expect_alive_check=True):
    t0 = time.time()
    try:
        resp = bytes(c.control(ESC, bytes_))
        dt = (time.time() - t0) * 1000
        print(f"[{dt:7.1f} ms] {label:34s} -> {resp.hex()}")
        return resp
    except Exception as e:
        print(f"[  FAILED] {label:34s} !! {str(e)[:100]}")
        return None


def alive(c, label):
    r = ctl(c, label + " (fw probe)", [0xE0, 0, 0, 0x18, 0])
    return r is not None


# 256-byte T2T NDEF image: CC(E1 40 20 00) + TLV + text record with a
# 225-char stand-in token, padded — same shape as the e2e's image.
def build_image():
    text = b"cashuA" + b"x" * 219           # 225 chars like the stripped token
    payload = b"\x02en" + text
    record = bytes([0xD1, 0x01, len(payload), 0x54]) + payload
    tlv = bytes([0x03, len(record)]) + record
    img = bytes([0xE1, 0x40, 0x20, 0x00]) + tlv
    img += b"\x00" * (255 - len(img)) + b"\xFE"
    assert len(img) == 256
    return img


def main():
    img = build_image()

    print("== phase A: replicate the rig preflight (repair path) ==")
    c = connect()
    ctl(c, "A1 fw read", [0xE0, 0, 0, 0x18, 0])
    poll = ctl(c, "A2 polling read", [0xE0, 0x00, 0x00, 0x23, 0x00])
    if poll and poll[-1] == 0x00:
        ctl(c, "A3 polling restore 0x8F", [0xE0, 0x00, 0x00, 0x23, 0x01, 0x8F])
    c.disconnect()
    print("   (preflight connection dropped, as RigGuard does)")
    time.sleep(2)

    print("== phase B: replicate present_ndef_image over python T0+DIRECT ==")
    c = connect()
    if not ctl(c, "B1 quiet polling", [0xE0, 0x00, 0x00, 0x23, 0x01, 0x00]):
        return fail("quiet")
    time.sleep(1.0)
    alive(c, "B2")

    for i, off in enumerate(range(0, 256, 48)):
        chunk = img[off:off + 48]
        cmd = [0xE0, 0x00, 0x00, 0x60, len(chunk) + 4, 0x01, 0x01, off, len(chunk)] + list(chunk)
        if not ctl(c, f"B3.{i} write {len(chunk)}B @{off}", cmd):
            return fail(f"write @{off}")
    alive(c, "B4")

    rb = ctl(c, "B5 readback 48B @0", [0xE0, 0x00, 0x00, 0x60, 0x04, 0x00, 0x01, 0x00, 48])
    if rb is None:
        return fail("readback")
    body = rb[5:5 + 48] if len(rb) >= 53 else rb
    print("   readback matches image head:", body[:16] == img[:16])

    if not ctl(c, "B6 ENTER CE", [0xE0, 0x00, 0x00, 0x40, 0x03, 0x01, 0x00, 0x00]):
        return fail("enter")
    time.sleep(0.5)
    print("== phase C: does USB survive enter? ==")
    if not alive(c, "C1"):
        print("   USB dead after enter (the classic wedge)")
    else:
        print("   USB ALIVE after enter — checking whether the M5Stick sees the tag")
    c.disconnect()
    return True


def fail(step):
    print(f"\n>>> FIRST FAILING STEP: {step} — this is the wedge trigger")
    return False


if __name__ == "__main__":
    ok = main()
    sys.exit(0 if ok else 1)
