"""T2 — full wallet lifecycle vs the bench mint, console-asserted.

seed generate → mint add → (rig mints a real token host-side) →
`receive` → `balance` → `invoice` + `melt` → `wipe`. The mint is the
public fake-sat testnut (money taxonomy: testnut/fake = free rein);
the rig payer is the single minting truth on the bench.

Wipes the device wallet at both ends (setup AND teardown) so the suite
is order-independent and leaves no balance behind.
"""

import re

import pytest

RECEIVE_TIMEOUT_S = 120   # includes mint round-trips + swap
MELT_TIMEOUT_S = 180


@pytest.fixture()
def clean_wallet(nucula_console):
    nucula_console.cmd("seed wipe", timeout_s=30)
    nucula_console.cmd("seed generate", timeout_s=60)
    yield nucula_console
    nucula_console.cmd("seed wipe", timeout_s=30)


def _assert_contains(out: str, needle: str):
    assert needle in out, f"missing {needle!r} in:\n{out}"


def test_wallet_lifecycle_vs_bench_mint(clean_wallet, mint_url, repo_root):
    con = clean_wallet

    # -- mint enrollment --------------------------------------------------
    out = con.cmd(f"mint add {mint_url}", timeout_s=90)
    _assert_contains(out, f"added mint [0]: {mint_url}")
    out = con.cmd("mint", timeout_s=30)
    _assert_contains(out, mint_url)

    # -- host-side token from the rig payer (minting truth) ----------------
    from harness import mint_token_via_rig
    token = mint_token_via_rig(repo_root, mint_url, sats=21)

    # -- receive over the console (decode + swap at the mint) --------------
    out = con.cmd(f"receive {token}", timeout_s=RECEIVE_TIMEOUT_S)
    _assert_contains(out, "received ")
    _assert_contains(out, " in ")

    # -- balance -----------------------------------------------------------
    out = con.cmd("balance", timeout_s=30)
    m = re.search(r"total:\s*([0-9.\u2089\u20BF]+(?:\.\d+)?)", out) or \
        re.search(r"total:\s*(\S+)", out)
    assert m, out

    # -- melt leg: invoice then melt (auto-settling fake mint) -------------
    out = con.cmd("invoice 2", timeout_s=120)
    m = re.search(r"(lnbc[a-z0-9]+)", out)
    assert m, f"no bolt11 invoice printed:\n{out}"
    invoice = m.group(1)
    out = con.cmd(f"melt {invoice}", timeout_s=MELT_TIMEOUT_S)
    assert "error" not in out.lower(), out
    # melt consumed 2 sats (+ mint fee); balance must have shrunk but stayed > 0
    out = con.cmd("balance", timeout_s=30)
    m = re.search(r"total:\s*(\d+)", out)
    assert m and 0 < int(m.group(1)) < 21, out


def test_phone_receives_rig_token_over_browser_link(phone, token_link,
                                                    mint_url, repo_root,
                                                    evidence_dir):
    """The proven token hand-off as a regression lock: rig mints at the
    bench mint, phone receives via the browser-link page, balance sheet
    shows the amount."""
    from harness import mint_token_via_rig, BENCH_LAN_IP, BENCH_LINK_PORT
    token = mint_token_via_rig(repo_root, mint_url, sats=8)
    token_link.serve_token(token)

    phone.launch_cashu()
    phone.open_link_page_and_tap(BENCH_LAN_IP, BENCH_LINK_PORT)
    phone.save_dump("t2_after_link_tap")

    # Receive-Ecash sheet: amount parsed + a receive/confirm button
    deadline = __import__("time").monotonic() + 30
    seen = False
    while __import__("time").monotonic() < deadline:
        texts = phone.screen_texts()
        if any("8" in t for t in texts) and any(
                k in " ".join(texts).lower() for k in ("receive", "confirm")):
            seen = True
            break
        phone.tap_text("Receive", settle=3.0)
    evidence_dir.joinpath("t2_phone_receive_texts.log").write_text(
        "\n".join(phone.screen_texts()))
    assert seen, "phone did not show a receive sheet for the linked token"
