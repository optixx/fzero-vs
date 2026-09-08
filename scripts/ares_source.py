#!/usr/bin/env python3
"""Manage pinned Ares source without resetting or discarding local modifications."""

import argparse
import fcntl
import hashlib
import json
import os
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def verify(source, pin):
    if not (source / ".git").is_dir():
        raise RuntimeError(f"{source} exists but is not an Ares checkout; it was not modified")
    if git(source, "rev-parse", "HEAD") != pin["commit"]:
        raise RuntimeError("Ares HEAD differs from config/ares.json. Preserve your work and resolve the pin explicitly; no reset was performed.")
    if git(source, "remote", "get-url", "origin") != pin["repository"]:
        raise RuntimeError("Ares origin differs from config/ares.json; refusing to reuse this checkout")


def fetch(root, pin):
    source = root / "vendor/ares"
    if not source.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", pin["tag"],
                        pin["repository"], str(source)], check=True)
    verify(source, pin)
    print(f"Ares {pin['tag']} verified: {pin['commit']}")


def apply_patches(root, pin):
    source = root / "vendor/ares"
    verify(source, pin)
    directory = root / "patches/ares"
    files = []
    for line in (directory / "series").read_text().splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        path = (directory / name).resolve()
        if not path.is_relative_to(directory.resolve()) or not path.is_file():
            raise RuntimeError(f"Invalid or missing patch: {name}")
        if path in files:
            raise RuntimeError(f"Duplicate patch: {name}")
        files.append(path)
    signature = {"commit": pin["commit"], "patches": [
        {"file": str(p.relative_to(directory.resolve())), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in files]}
    marker = source / ".git/fzvs-patches.json"
    if marker.exists() and json.loads(marker.read_text()) == signature:
        print("Ares patch series already applied; local edits preserved.")
        return
    if git(source, "status", "--porcelain"):
        raise RuntimeError("Ares has local changes or a different applied patch series. Preserve/export them and prepare a clean checkout before changing the series; no files were reset.")
    if files:
        # Apply dependent patches to a temporary index first. If any patch fails,
        # the real index and working tree are untouched. Then apply one net diff.
        command = ["git", "-C", str(source)]
        with tempfile.TemporaryDirectory(prefix="fzvs-patch-check-") as temp:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / "index"))
            subprocess.run(command + ["read-tree", "HEAD"], env=env, check=True)
            for path in files:
                subprocess.run(command + ["apply", "--cached", str(path)], env=env, check=True)
            combined = subprocess.check_output(command + [
                "diff", "--cached", "--binary", "--no-ext-diff", "--no-textconv",
                "--no-color", "--src-prefix=a/", "--dst-prefix=b/", "HEAD"], env=env)
        if combined:
            subprocess.run(command + ["apply", "--index", "--check"], input=combined, check=True)
            subprocess.run(command + ["apply", "--index"], input=combined, check=True)
    marker.write_text(json.dumps(signature, indent=2) + "\n")
    print(f"Applied {len(files)} Ares patches." if files else
          "Patch series empty: building stock Ares; multiplayer flags are not available yet.")


def link_binary(root):
    build = root / "build/ares"
    executable = (build / "desktop-ui/ares.app/Contents/MacOS/ares" if sys.platform == "darwin"
                  else build / "desktop-ui/ares")
    if not executable.is_file():
        raise RuntimeError(f"Built Ares executable not found: {executable}")
    link = root / "build/bin/ares"
    link.parent.mkdir(parents=True, exist_ok=True)
    header = "#!/bin/sh\n# Generated FZVS Ares launcher; managed by scripts/ares_source.py.\n"
    if link.is_symlink():
        link.unlink()
    elif link.exists() and not link.read_text().startswith(header):
        raise RuntimeError(f"Refusing to overwrite unmanaged launcher: {link}")
    # NSBundle uses the launched executable path to locate Metal shaders. A
    # symlink in build/bin makes it look there instead of inside the app bundle.
    # exec preserves signals/PID, argv quoting, and the caller's working directory.
    relative = shlex.quote(os.path.relpath(executable, link.parent))
    link.write_text(header + 'launcher_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
                    + 'exec "$launcher_dir"/' + relative + ' "$@"\n')
    link.chmod(0o755)
    print(f"Harness executable: {link}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "patch", "link"))
    args = parser.parse_args()
    try:
        pin = json.loads((ROOT / "config/ares.json").read_text())
        (ROOT / "vendor").mkdir(exist_ok=True)
        # Serialize independent make invocations touching the source checkout.
        with (ROOT / "vendor/.ares-source.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if args.action == "fetch":
                fetch(ROOT, pin)
            elif args.action == "patch":
                apply_patches(ROOT, pin)
            else:
                link_binary(ROOT)
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"Ares setup failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
