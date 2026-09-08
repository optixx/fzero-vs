#!/usr/bin/env python3
"""Supervise a local server and independent clients; see docs/local-testing.md."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ROM = ROOT / "roms/F-ZERO (U) [!].smc"


def positive(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return number


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--players", type=int, choices=(2, 3, 4), default=2)
    parser.add_argument("--client", type=Path, default=ROOT / "build/bin/ares")
    parser.add_argument("--server-mode", choices=("hosted", "standalone"), default="hosted")
    parser.add_argument("--server", type=Path, default=ROOT / "build/server/fzvs-server")
    parser.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    parser.add_argument("--port", type=int, default=12000)
    parser.add_argument("--run-root", type=Path, default=ROOT / ".local-tests")
    parser.add_argument("--startup-timeout", type=positive, default=10.0)
    parser.add_argument("--duration", type=positive, help="stop after this many seconds once clients launch")
    parser.add_argument("--input", action="append", dest="inputs", metavar="DEVICE",
                        help="repeat once per client: keyboard, none, or gamepad:N (zero-based)")
    parser.add_argument("--dry-run", action="store_true", help="print launch plan without writing files or starting processes")
    parser.add_argument("--track", type=int, choices=range(5), default=0)
    parser.add_argument("--league", type=int, choices=range(3), default=0)
    parser.add_argument("--car", type=int, choices=range(4), help="use the same scripted car 0..3 in every client")
    parser.add_argument("--baseline", action="store_true", help="diagnostic: disable all game memory patches")
    parser.add_argument("--auto-drive", action="store_true", help="script car selection and acceleration in each client")
    parser.add_argument("--capture-frames", action="store_true", help="save one diagnostic image per race in each client profile")
    parser.add_argument("--auto-next", action="store_true", help="host automatically starts the next lobby after results")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.port == 12001:
        parser.error("--port must be between 1 and 65535, excluding discovery port 12001")
    if args.inputs is None:
        args.inputs = ["keyboard"] + ["none"] * (args.players - 1)
    if len(args.inputs) != args.players:
        parser.error("supply exactly one --input per client")
    if any(not re.fullmatch(r"keyboard|none|gamepad:[0-9]+", item) for item in args.inputs):
        parser.error("invalid input: use keyboard, none, or gamepad:N")
    active = [item for item in args.inputs if item != "none"]
    if len(active) != len(set(active)):
        parser.error("assign each active input device to only one client")
    for field in ("client", "server", "rom", "run_root"):
        setattr(args, field, getattr(args, field).expanduser().resolve())
    return args


def launch_plan(args, run_dir):
    # Test-only room credential; do not reuse a real server credential here.
    room_key = "local-test"
    ready_file = run_dir / "server.ready"
    server = [str(args.server), "--bind", "127.0.0.1", "--port", str(args.port),
              "--players", str(args.players), "--track", str(args.track), "--league", str(args.league), "--room-key", room_key,
              "--log-level", "debug", "--ready-file", str(ready_file)]
    entries = [{"name": "server", "argv": server, "cwd": str(run_dir / "server"),
                "log": str(run_dir / "server.log")}]
    if args.server_mode == "hosted":
        entries = []
    for index in range(args.players):
        name = f"client-{index + 1}"
        profile = run_dir / name
        x, y = 40 + (index % 2) * 680, 60 + (index // 2) * 560
        client = [str(args.client), "--fzvs-server", f"127.0.0.1:{args.port}",
                  "--fzvs-room-key", room_key, "--fzvs-profile", str(profile),
                  "--fzvs-label", f"F-Zero VS — Client {index + 1}",
                  "--fzvs-input", args.inputs[index],
                  "--fzvs-window", f"{x},{y},640,480", "--fzvs-stay-active"]
        if args.server_mode == "hosted" and index == 0:
            client[1:3] = ["--fzvs-host", "--fzvs-players", str(args.players),
                           "--fzvs-bind", "127.0.0.1", "--fzvs-port", str(args.port),
                           "--fzvs-room-name", "Local test", "--fzvs-track", str(args.track),
                           "--fzvs-league", str(args.league), "--fzvs-host-ready-file", str(ready_file)]
        if index:
            client.append("--fzvs-mute")
        if args.baseline:
            client += ["--fzvs-baseline"]
        if args.auto_drive:
            client.extend(["--fzvs-auto-car", str(index if args.car is None else args.car)])
        if args.capture_frames:
            client.append("--fzvs-capture")
        if args.auto_next:
            client.append("--fzvs-auto-next")
        client.append(str(args.rom))
        entries.append({"name": name, "argv": client, "cwd": str(profile),
                        "log": str(run_dir / f"{name}.log")})
    return {"server_mode": args.server_mode, "players": args.players, "server_ready_file": str(ready_file), "processes": entries}


def start(entry, children):
    Path(entry["cwd"]).mkdir(parents=True, exist_ok=True)
    # Each process gets its own process group so helper processes also get stopped.
    with open(entry["log"], "wb") as output:
        child = subprocess.Popen(entry["argv"], cwd=entry["cwd"], stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    children.append((entry["name"], child))
    print(f"Started {entry['name']} pid={child.pid}; log={entry['log']}", flush=True)


def signal_group(child, signum):
    try:
        os.killpg(child.pid, signum)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Some restricted macOS environments report EPERM for an exited group.
        # A still-running owned child must still receive the shutdown signal.
        if child.poll() is None:
            child.send_signal(signum)


def stop(children):
    for _, child in children:
        signal_group(child, signal.SIGTERM)
    deadline = time.monotonic() + 3
    while any(child.poll() is None for _, child in children) and time.monotonic() < deadline:
        time.sleep(0.05)
    for _, child in children:
        signal_group(child, signal.SIGKILL)
        child.wait()


def main():
    args = arguments()
    if args.dry_run:
        plan = launch_plan(args, args.run_root / "<new-session>")
        print("DRY RUN: launch plan for the patched Ares client and C server.")
        for entry in plan["processes"]:
            print(f"\n{entry['name']} (cwd={entry['cwd']}):\n  {shlex.join(entry['argv'])}")
            print(f"  log: {entry['log']}")
        print(f"\nWait for server readiness: {plan['server_ready_file']}")
        return 0

    for label, path in [("client", args.client)] + ([("server", args.server)] if args.server_mode == "standalone" else []):
        if not path.is_file() or not os.access(path, os.X_OK):
            print(f"Missing executable {label}: {path}\n"
                  "Build the F-Zero VS binaries or supply explicit paths; use --dry-run to inspect the setup.",
                  file=sys.stderr)
            return 2
    if not args.rom.is_file():
        print(f"ROM not found: {args.rom}; supply --rom PATH", file=sys.stderr)
        return 2

    args.run_root.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=args.run_root))
    plan = launch_plan(args, run_dir)
    (run_dir / "launch.json").write_text(json.dumps(plan, indent=2) + "\n")
    print(f"Session: {run_dir}", flush=True)
    children = []
    result = {"reason": "startup_error", "exit_code": 1}

    def interrupted(signum, _frame):
        raise KeyboardInterrupt

    previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        start(plan["processes"][0], children)
        deadline = time.monotonic() + args.startup_timeout
        ready = Path(plan["server_ready_file"])
        while True:
            if children[0][1].poll() is not None:
                raise RuntimeError("Host/server exited before readiness; see client-1.log or server.log")
            if ready.is_file():
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Host/server readiness timed out; see client-1.log or server.log")
            time.sleep(0.05)
        for entry in plan["processes"][1:]:
            start(entry, children)
        print("Test setup running. Close any instance or press Ctrl+C to stop all processes.", flush=True)
        deadline = time.monotonic() + args.duration if args.duration else None
        while True:
            ended = next(((name, child.returncode) for name, child in children if child.poll() is not None), None)
            if ended:
                name, code = ended
                result = {"reason": "process_exit", "process": name, "process_exit_code": code,
                          "exit_code": 0 if name != "server" and code == 0 else 1}
                print(f"{name} exited ({code}); stopping the setup.", flush=True)
                break
            if deadline is not None and time.monotonic() >= deadline:
                result = {"reason": "duration_elapsed", "exit_code": 0}
                break
            time.sleep(0.05)
    except KeyboardInterrupt:
        result = {"reason": "interrupted", "exit_code": 130}
    except (OSError, RuntimeError) as error:
        result = {"reason": "error", "message": str(error), "exit_code": 1}
        print(str(error), file=sys.stderr)
    finally:
        # Further Ctrl+C presses must not interrupt cleanup and leave children behind.
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        stop(children)
        result["processes"] = [{"name": name, "pid": child.pid, "exit_code": child.returncode}
                               for name, child in children]
        (run_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        print(f"Stopped. Logs retained in {run_dir}", flush=True)
    return result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
