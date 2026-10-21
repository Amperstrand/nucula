"""Fixtures for the phone-vs-device HIL suites (nucula).

--hil gates every hardware touch (ccid-firmware-rs idiom): plain
`pytest tests/` in dev/PR CI touches nothing; the bench runner and the
operator pass --hil explicitly.

Suite-level exclusivity: the nucula place fixture acquires `nucula-c3`
for the whole module and releases it on teardown; the console opens
ONCE per module (each fresh open of the C3 CDC resets the board —
harness.Console docstring).
"""

from __future__ import annotations

import datetime
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).parent
from harness import (  # noqa: E402
    AcquiredPlace,
    Console,
    LabgridBusy,
    Phone,
    TokenLinkServer,
    resolve_serial_port,
)

RESULTS_ROOT = Path(__file__).parent.parent.parent / "hil-results"


def pytest_addoption(parser):
    parser.addoption("--hil", action="store_true", default=False,
                     help="drive real bench hardware (labgrid/adb/serial)")
    parser.addoption("--mint-url", action="store",
                     default="https://testnut.cashu.space",
                     help="mint the rig pays from (fake-sat testnut default)")


def _need_hil(request) -> None:
    if not request.config.getoption("--hil"):
        pytest.skip("hardware gated: pass --hil on the bench runner")


# ---------------------------------------------------------------- evidence

@pytest.fixture(scope="session")
def evidence_dir() -> Path:
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    d = RESULTS_ROOT / stamp
    d.mkdir(parents=True, exist_ok=True)
    return d


# ---------------------------------------------------------------- places

@pytest.fixture(scope="module")
def nucula_place(request):
    _need_hil(request)
    try:
        with AcquiredPlace("nucula-c3") as place:
            yield place
    except LabgridBusy as e:
        pytest.skip(f"bench busy: {e}")


@pytest.fixture(scope="module")
def phone_place(request):
    _need_hil(request)
    try:
        with AcquiredPlace("phone-s20") as place:
            yield place
    except LabgridBusy as e:
        pytest.skip(f"bench busy: {e}")


@pytest.fixture(scope="module")
def acr_place(request):
    _need_hil(request)
    try:
        with AcquiredPlace("ref-acr1252") as place:
            yield place
    except LabgridBusy as e:
        pytest.skip(f"bench busy: {e}")


# ---------------------------------------------------------------- devices

@pytest.fixture(scope="module")
def nucula_console(request, nucula_place, evidence_dir) -> Console:
    """One console session per module: open (board resets — expected),
    sync on the first `nucula> ` prompt, teardown saves the transcript.

    Two guards keep this honest on a shared bench:
    - the resolved port must have no foreign holder (the ccid broker
      keeps the CDC open forever — two opens of one tty do NOT exclude
      each other and the second one resets the board mid-session);
    - a banner timeout SKIPS naming what booted instead of erroring:
      the board may be serving another project's image (ccid GemPC).
    """
    port = resolve_serial_port("nucula-c3")
    holders = subprocess.run(
        ["fuser", "-v", port], capture_output=True, text=True)
    if holders.returncode == 0 and holders.stderr.strip():
        pytest.skip(
            f"console port held by a foreign process ({holders.stderr.strip()})"
            " — e.g. nucula-ccid-broker; stop it per tests/hil/phone/"
            "README.md (the broker swap dance) before wallet-fw suites")
    from harness import Console
    con = Console(port, label="nucula", evidence_dir=evidence_dir)
    request.addfinalizer(con.close)
    try:
        banner = con.boot_banner()
    except TimeoutError as e:
        pytest.skip(
            "board not on nucula wallet firmware (no prompt in 20 s — "
            f"foreign image or wedged console): {e}")
    (evidence_dir / "nucula.banner.log").write_text(banner)
    return con


@pytest.fixture(scope="module")
def phone(request, phone_place, evidence_dir) -> Phone:
    _need_hil(request)
    ph = Phone(evidence_dir=evidence_dir)
    # assert adb transport actually alive before anything else
    out = ph.sh("getprop ro.product.model")
    assert "SM-G780" in out, f"unexpected phone on {ph.serial}: {out!r}"
    ph.keep_awake()
    request.addfinalizer(lambda: ph.save_dump("phone.final"))
    return ph


@pytest.fixture(scope="module")
def token_link(request, tmp_path_factory) -> TokenLinkServer:
    try:
        srv = TokenLinkServer(tmp_path_factory.mktemp("tokenserve"))
        srv.start()
    except OSError as e:
        pytest.skip(
            f"bench link port busy ({e}) — a standing token server owns "
            "8077; reuse it or free the port before browser-link tests")
    request.addfinalizer(srv.stop)
    return srv


@pytest.fixture(scope="module")
def mint_url(request) -> str:
    return request.config.getoption("--mint-url")


@pytest.fixture(scope="module")
def repo_root() -> Path:
    return HERE.parent.parent.parent
