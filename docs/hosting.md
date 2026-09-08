# Hosting and joining from Ares

Open **Multiplayer → F-Zero VS…**. The menu is available without
network flags and before loading a game.

The resizable 640×560 window has **Play** and **Diagnostics** tabs. Play shows
Host/Join setup until connected, then your room. Closing this window only hides
it. Startup keeps the form visible with disabled editing and a Cancel action.

**Host** chooses 2–4 total players, a room name and one of the fifteen tracks.
Defaults are two players, Mute City I, a computer-name room title, all IPv4
interfaces and UDP 12000. Bind address and port are under Advanced. Leave the
password empty for an open room. The hosting emulator runs the C11 server on a
worker thread, connects locally as reserved P1, then admits guests and advertises.
Choose **Create room** to start. Race settings stay visible in the host Room
view; editing is available in the lobby. **Apply changes** requires valid changes,
and current configuration reflects server acknowledgements. **Next race** appears
after results. Machine selection still starts the race automatically when everyone
is ready. The emulator receives focus once after connecting.
Player color identifies the network slot (P1 pink, P2 blue, P3 green, P4 yellow);
the selected machine is shown separately. Machine selection does not choose your
multiplayer paint color.

**Join** browses nearby rooms every two seconds and removes entries after six
seconds without a reply. Rooms show name, source address/game port, occupancy,
track, password requirement and availability. Select a room or enter
a hostname or IPv4 address (port 12000 by default), or `hostname:port`. Direct joining works independently of discovery. Incorrect
password, full room, race underway, incompatible protocol, DNS failure, invalid
ROM and occupied-port errors appear in the window.

Networking requires the original US F-Zero ROM. Ares first checks the loaded
ROM, then its remembered F-Zero path, then opens a file picker. It verifies the
SHA-256 before connecting. Preferences and the last successful join address are
saved in the profile; passwords and an active hosting state are not saved.

Stop hosting ends the room. Guest Leave restores ROM patches and the previous
local pause, audio, defocus, rewind, run-ahead and debugger settings. ROM unload,
replacement and normal application exit also close the room, close sockets and
join the server worker. Internal race resets keep the room alive. Guests receive
an authenticated close notification; a lost notification falls back to the
five-second connection timeout. Hosted rooms have no host migration. Dedicated
servers retain their existing host reassignment behavior.

Diagnostics groups Connection, Players and Hosted server metrics. Disconnected
views show guidance instead of zero-filled statistics. Technical details contains
WRAM and packet counters. The bounded log supports severity filtering and pause,
and preserves selection/scroll position while updating. Discovery diagnostics
include socket operation, interface and OS error; **Retry** refreshes interfaces
and recreates discovery sockets without ending the room. Successful queries do
not imply that replies were received. Host events also
append to `host-server.log` beside the profile's settings file. Server events do
not use SNES instruction tracing. The footer has one updater and the HUD still
shows only RTT, jitter and loss.

## Command-line equivalents

Host:

```sh
./build/bin/ares --fzvs-host --fzvs-players 2 \
  --fzvs-room-name 'Local game' --fzvs-bind 0.0.0.0 --fzvs-port 12000 \
  --fzvs-track 0 --fzvs-league 0 --fzvs-profile "$PWD/.local-tests/manual/host" \
  'roms/F-ZERO (U) [!].smc'
```

Guest (replace the address for another machine):

```sh
./build/bin/ares --fzvs-server 127.0.0.1:12000 \
  --fzvs-profile "$PWD/.local-tests/manual/guest" 'roms/F-ZERO (U) [!].smc'
```

Add `--fzvs-room-key PASSWORD` to both if needed. Automation may supply
`--fzvs-host-ready-file PATH`; it is written only after P1 registers. Existing
input, label, window, capture and logging options remain available.

```sh
./scripts/test-2p.sh --auto-drive --capture-frames --duration 35
./scripts/test-4p.sh --auto-drive --capture-frames --track 4 --league 2 --duration 35
./scripts/test-2p.sh --server-mode standalone --duration 35
```

Internet joining needs a reachable game UDP port. Discovery is LAN-only. Public
matchmaking, NAT traversal, IPv6 and host migration are outside this milestone.
Room passwords use the existing unencrypted room-key mechanism.

## Implementation and verification

The same C11 library backs the standalone executable and Ares. Each server has
one owner thread and exposes bounded polling (128 packets, at most 16 ms), status
snapshots and a 256-entry event ring. The session controller has an eight-command
queue and a 512-event log. It owns server/discovery sockets and never touches
emulator memory or widgets. The GUI consumes copied snapshots; the existing
emulation integration owns gameplay updates. Portable DNS cancellation discards
results from at most four resolver tasks that own no server, UI or emulator state.

`make test` includes protocol fuzz cases, two-race memory and fault tests,
standalone regression tests, reservation and rejection checks, repeated host
cycles, DNS cancellation/errors, occupied ports, concurrent room binding,
authenticated close, lost-close timeout, ROM restoration and harness cleanup.
Set `FZVS_REQUIRE_MULTICAST=1 make test-session` on a multicast-capable machine to
require discovery of both local rooms and six-second expiry rather than report a
multicast acceptance skip.

Real hosted two-player Mute City I and four-player Fire Field runs reached racing
and produced clean captures on this Mac. A 150-second hosted run remained stable,
but straight-line driving did not finish a race; repeated races are covered by
the synthetic memory test, not claimed as a completed desktop acceptance run. Native UI checks cover hosting, acknowledged track changes, cancellation, one-time
game focus, compact light/dark layouts, Stop hosting and normal quit. Session tests
cover restoration and shutdown; unload during an active native UI session remains
a manual acceptance check. Local
multicast reception was unavailable even in a separate minimal socket probe;
two-machine LAN discovery and manual direct joining with multicast blocked remain
acceptance checks, not claimed passes. Linux source compatibility is retained but
a Linux Ares build has not been executed here.

Native tab teardown regression: `make test-ui-tabs` (macOS desktop required)
checks removal, reattachment, explicit reset and destruction across 50 windows.
The previous tab removal detached its item before using its index, producing the
reported shutdown crash. The corrected ordering passes this test; a hosted Ares
normal quit also returned exit code 0 and removed its readiness file.

`make test-ux` checks deterministic room presentation, default addresses, injected
discovery failures, retry/interface refresh and stale-warning removal.
`make test-ui-text` checks native wrapped log width, selection and scroll
preservation, including recovery when the filtered log becomes shorter.
