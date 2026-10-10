#!/usr/bin/env python3
"""Run the compiled hook self-test in isolated Vita3K storage (no device)."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vita3k", default="vita3k")
    parser.add_argument("--vpk", type=Path, default=Path("build/vita/vjo-hook-test.vpk"))
    parser.add_argument("--workdir", type=Path, default=Path("build/vita3k-hook-test"))
    args = parser.parse_args()
    if not args.vpk.is_file():
        parser.error("build with -DVJO_TEXT_TEST_APP=ON first")
    if not shutil.which("xvfb-run") or not shutil.which(args.vita3k):
        parser.error("Vita3K and xvfb-run must be installed")
    root = args.workdir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    marker = root / ".vjo-hook-test"
    if not marker.exists() and any(root.iterdir()):
        parser.error("workdir is nonempty and does not belong to this test")
    marker.touch()
    env = os.environ.copy()
    for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache")):
        path = root / name
        path.mkdir(exist_ok=True)
        env[key] = str(path)
    pref = root / "pref"
    pref.mkdir(exist_ok=True)
    config = root / "test.yml"
    config.write_text("\n".join([
        "initial-setup: true", "backend-renderer: OpenGL", "log-level: 2",
        "show-welcome: false", "warn-missing-firmware: false", "check-for-updates-mode: 0",
        "discord-rich-presence: false", "show-live-area-screen: false",
        "pref-path: " + json.dumps(str(pref) + "/"), "",
    ]))
    result = pref / "ux0/data/VitaJPOverlay-hook-test/results.txt"
    result.unlink(missing_ok=True)
    env.update(LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", args.vita3k,
               "--frontend", "sdl", "-c", str(config), str(args.vpk.resolve())]
    with (root / "stdout.log").open("w") as log:
        try:
            run = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=55)
        except subprocess.TimeoutExpired:
            parser.exit(1, f"Vita3K timed out; inspect {root / 'stdout.log'}\n")
    output = result.read_text() if result.exists() else ""
    print(output, end="")
    groups = re.findall(r"^(portable|observer|plugin) checks=(\d+) failures=(\d+)", output, re.M)
    if run.returncode or len(groups) != 3 or any(int(g[2]) for g in groups) or output.splitlines()[-1:] != ["RESULT PASS"]:
        parser.exit(1, f"Hook test failed; inspect {root / 'stdout.log'}\n")
    print(f"Vita3K passed {sum(int(g[1]) for g in groups)} checks. Evidence: {result}")


if __name__ == "__main__":
    main()
