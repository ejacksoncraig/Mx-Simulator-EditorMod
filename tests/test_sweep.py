"""The editor mod's radial sweep (src/sweep.c) against the game's own radial rule and the wedge rule.

Builds sweep.c with MSVC (skipped when Visual Studio Build Tools aren't installed), the way mod/build.bat finds it.
"""
import math
import os
import random
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent  # the mod's folder
VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"


@pytest.fixture(scope="module")
def sweep(tmp_path_factory):
    if os.name != "nt" or not VSWHERE.exists():
        pytest.skip("needs MSVC (Visual Studio Build Tools)")
    vs = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-requires",
                         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                        capture_output=True, text=True).stdout.strip()
    if not vs:
        pytest.skip("no MSVC C++ tools")
    out = tmp_path_factory.mktemp("sweep")
    bat = out / "build.bat"
    bat.write_text(
        f'@call "{vs}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\n'
        f'@cl /nologo /O2 /Fe"{out / "sweep.exe"}" /Fo"{out}\\\\" "{ROOT / "src/sweep.c"}" '
        f'"{ROOT / "tests/sweep_driver.c"}"\r\n')
    built = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, cwd=out)
    assert built.returncode == 0, built.stdout + built.stderr

    def run(cases):
        text = "".join(f"{vx!r} {vz!r} {mid!r} {half!r}\n" for vx, vz, mid, half in cases)
        got = subprocess.run([str(out / "sweep.exe")], input=text, capture_output=True, text=True, check=True).stdout
        return [float(v) for v in got.split()]

    yield run
    shutil.rmtree(out, ignore_errors=True)


def game_s(vx, vz, dx, dz):
    """mx.exe 0x14005fa10: behind A, v loses its part along A to B; s = |v|."""
    if dx * vx + dz * vz < 0:
        inv = 1 / math.sqrt(dx * dx + dz * dz)
        ux, uz = dx * inv, dz * inv
        along = vx * ux + vz * uz
        vx, vz = vx - ux * along, vz - uz * along
    return math.sqrt(vx * vx + vz * vz)


def wedge_s(vx, vz, mid, half):
    """The wedge rule, by vectors rather than angles: inside, the distance; outside, along the nearer edge."""
    s = math.hypot(vx, vz)
    if s == 0:
        return 0.0
    mx, mz = math.cos(mid), math.sin(mid)
    if (vx * mx + vz * mz) / s >= math.cos(half) - 1e-12:
        return s
    clockwise = mx * vz - mz * vx > 0  # the side a growing angle turns to
    e = mid + (half if clockwise else -half)
    return max(0.0, vx * math.cos(e) + vz * math.sin(e))


def points(n, seed):
    rnd = random.Random(seed)
    return [(rnd.uniform(-200, 200), rnd.uniform(-200, 200)) for _ in range(n)]


def test_180_centred_is_the_games_own_rule(sweep):
    rnd = random.Random(1)
    cases, want = [], []
    for vx, vz in points(4000, 2):
        dx, dz = rnd.uniform(-100, 100), rnd.uniform(-100, 100)
        cases.append((vx, vz, math.atan2(dz, dx), math.pi / 2))
        want.append(game_s(vx, vz, dx, dz))
    for got, w in zip(sweep(cases), want):
        assert got == pytest.approx(w, abs=1e-9)


def test_full_circle_is_the_distance(sweep):
    cases = [(vx, vz, 0.7, math.pi) for vx, vz in points(500, 3)]
    for (vx, vz, _, _), got in zip(cases, sweep(cases)):
        assert got == pytest.approx(math.hypot(vx, vz), abs=1e-9)


@pytest.mark.parametrize("degrees", [30, 90, 135, 270, 345])
def test_wedges_follow_their_edges(sweep, degrees):
    half = math.radians(degrees) / 2
    cases = [(vx, vz, mid, half) for (vx, vz), mid in zip(points(2000, degrees), [0.3, -2.0, 3.1, 1.2] * 500)]
    for (vx, vz, mid, h), got in zip(cases, sweep(cases)):
        assert got == pytest.approx(wedge_s(vx, vz, mid, h), abs=1e-9)


def test_clockwise_from_b_starts_at_b(sweep):
    # A 90 degree sweep clockwise from B = +x: its middle is at +45 degrees, so +x and +z (down the screen) are its
    # edges, a point at 30 degrees is inside, one at -30 degrees runs straight off the +x edge.
    mid, half = math.radians(45), math.radians(45)
    inside = (math.cos(math.radians(30)) * 50, math.sin(math.radians(30)) * 50)
    outside = (math.cos(math.radians(-30)) * 50, math.sin(math.radians(-30)) * 50)
    behind = (-50.0, -50.0)
    got = sweep([(*inside, mid, half), (*outside, mid, half), (*behind, mid, half)])
    assert got[0] == pytest.approx(50)
    assert got[1] == pytest.approx(50 * math.cos(math.radians(30)))
    assert got[2] == 0
