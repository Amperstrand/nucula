"""T1 — nucula on-device selftest over the labgrid-held console."""


def test_selftest_passes(nucula_console):
    out = nucula_console.cmd("selftest", timeout_s=90)
    assert "self-tests PASSED" in out, out


def test_status_reports_wifi_and_mints(nucula_console):
    out = nucula_console.cmd("status", timeout_s=30)
    assert "wifi:" in out and "nfc:" in out and "mints:" in out, out


def test_heap_sane(nucula_console):
    out = nucula_console.cmd("heap", timeout_s=30)
    # PSRAM-equipped board; anything under a few hundred KB free would
    # already have failed selftest — this is a canary, not a budget.
    m = out and __import__("re").search(r"free:\s+(\d+)", out)
    assert m, out
    assert int(m.group(1)) > 100_000, out
