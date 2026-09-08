# Local multiplayer test harness

The harness starts one loopback-only server and 2–4 independent Ares processes.
Build both components with `make all`. The networking flags require this patched
build; stock Ares does not support them.

## Launch presets

From the project root:

```sh
./scripts/test-2p.sh --dry-run
./scripts/test-4p.sh --dry-run
```

To play manually:

```sh
./scripts/test-2p.sh --client /path/to/ares --server /path/to/fzvs-server --rom '/path/to/F-ZERO.sfc'
./scripts/test-4p.sh --client /path/to/ares --server /path/to/fzvs-server \
  --input keyboard --input gamepad:0 --input gamepad:1 --input gamepad:2
```

Use the executable inside an Ares `.app` bundle, not the bundle directory. Paths
are resolved from the invoking directory; the wrappers work from any directory.
Default binaries are `build/bin/ares` and `build/server/fzvs-server`. The default
ROM is `roms/F-ZERO (U) [!].smc`.

Options include `--players 3`, `--port 12001` for a second concurrent setup,
`--startup-timeout 20`, `--duration 60`, and `--run-root /path/to/logs`.
Each run creates a unique directory under `.local-tests/` containing `launch.json`,
`result.json`, server/client logs, and independent working/profile directories.
Dry runs write nothing and do not require binaries or a ROM to exist.

Only client 1 has audio enabled. By default it uses the keyboard and the other
clients have input disabled. Assign gamepads explicitly to drive multiple cars;
use `--auto-drive` for scripted menu navigation and straight-line acceleration. Keyboard input
is focus-sensitive, but all clients must continue emulating while unfocused.
The launch order does not imply a server player ID: windows are labelled
**Client 1–4**, and the multiplayer UI shows the actual assigned **P1–P4**.

Windows request 640×480 game viewports in a two-column layout. Ares clamps
window placement to the available desktop when the requested grid will not fit.
Closing any instance stops the whole setup. Ctrl+C/SIGTERM also cleans up all
launched process groups, escalating to SIGKILL after three seconds. Logs remain
available. The supervisor does not kill processes by name or touch other sessions.

## Binary integration contract

The server accepts:

| Flag | Required behavior |
| --- | --- |
| `--bind 127.0.0.1` | Bind locally for desktop tests. |
| `--port N` | Listen on the selected UDP port; fail clearly if unavailable. |
| `--players N` | Configure the expected participant count. |
| `--room-key local-test` | Use a test-only room key. |
| `--log-level debug` | Emit timestamped session/player/state diagnostics to stdout/stderr. |
| `--ready-file PATH` | Create this file only after socket binding and room initialization succeed. |

The server must not daemonize. Any readiness-file write failure is a startup
failure. The harness waits for the file before launching clients, with a timeout
and server-exit detection. A unique session directory prevents stale readiness.

The Ares fork accepts a positional ROM path and:

| Flag | Required behavior |
| --- | --- |
| `--fzvs-server HOST:PORT` | Enable multiplayer and connect to this server. |
| `--fzvs-room-key KEY` | Join using the test room key. |
| `--fzvs-profile DIR` | Store all settings, saves, and mutable per-instance files here; never alongside the ROM or in the shared default profile. |
| `--fzvs-label TEXT` | Label the window; show assigned player identity separately. |
| `--fzvs-input DEVICE` | Restrict gameplay input to `keyboard`, `none`, or `gamepad:N` (zero-based device index). |
| `--fzvs-window X,Y,W,H` | Request window position and game viewport size in desktop logical coordinates. |
| `--fzvs-stay-active` | Continue emulation while unfocused. |
| `--fzvs-mute` | Mute host playback without stopping emulated audio execution. |

Each client must remain a separate foreground process; no single-instance
forwarding or detached child launcher. Runtime patches modify only its loaded
ROM copy. Emit state changes and networking diagnostics to stdout/stderr. Keep
room credentials out of diagnostic logs. `launch.json` includes the **test-only**
key for reproducibility; do not use this harness to store production credentials.

## Validation boundary

Harness smoke tests use temporary stand-in processes to check launch arguments,
readiness, log/profile separation, early exit, timeout, and signal cleanup.
Run them with `python3 scripts/test_local_harness.py`.
They do not establish emulator compatibility or a playable multiplayer race.
Real-binary acceptance requires two- and four-window tests, input isolation,
unfocused execution, profile isolation, audio behavior, and repeated races.

## Automated desktop checks

```sh
make test-4p RUN_ARGS='--auto-drive --capture-frames --duration 45'
```

`--capture-frames` writes a game image (including overlay) after five seconds of
racing in each client profile. `--auto-next` advances the host after all cars
finish. The test driver accelerates without steering; it is not a race-playing AI.
`--car 0` selects the same machine in every scripted client. `--track 4 --league 2`
selects Fire Field (track 0–4, league 0–2).
`--baseline` disables all ROM/WRAM patches for a graphics comparison with normal
single-player execution. It cannot establish a multiplayer race.

Open **Multiplayer → Players and diagnostics** for roster, host controls, race
configuration, raw game state, metrics and recent events. The single-line overlay
shows only RTT, jitter and estimated packet loss, including in fullscreen. Player
identity and game state appear in the window title/footer. Host **Next race** returns everyone to a fresh lobby.
Keyboard: arrows, Z=B, X=A, A=Y, S=X, Q=L, W=R, Return=Start, Space=Select.
Gamepads use normal Ares button mappings, filtered to the selected device index.
Pause, rewind, savestates and fast-forward are disabled while networking is active.

For a private remote server, run `build/server/fzvs-server --bind 0.0.0.0
--port 12000 --players 4 --room-key YOUR_KEY --log-level debug` (one shell line),
allow that UDP port on its host/router, and connect clients using the server's
reachable address. `--json-log PATH` appends JSON lines; `--log-level trace
--trace-player 2` traces P2 positions. The protocol is IPv4 and is not encrypted.
