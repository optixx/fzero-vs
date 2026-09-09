# F-Zero VS implementation

This is a position-replication port of the original patched Snes9x/C# system,
not input-lockstep SNES netplay. Each emulator owns its driver's physics, energy,
laps and finish detection. The server assigns identities and coordinates the
lobby/load/start/results barriers, then relays the latest positions at 60 Hz.
Local collisions affect the local car. Remote cars are rendered from a small client-side snapshot buffer: the client displays them 50--100 ms behind estimated server time, linearly interpolating position and taking the shortest circular path for orientation. The delay adapts as `50 ms + 2 * jitter`, clamped to that range. When a later snapshot is unavailable, the latest state is held; this protocol version deliberately does not extrapolate velocity.

## Transport

IPv4 UDP, maximum datagram 256 bytes. All integers explicitly little endian;
C structs are never sent directly. The 36-byte header is:

| Offset | Field |
|---|---|
| 0 | ASCII FZVS (4 bytes) |
| 4 | Version 1 (u8) |
| 5 | Message type (u8) |
| 6 | Payload length (u16) |
| 8 | Server session (u64) |
| 16 | Race generation (u32) |
| 20 | Sequence (u32) |
| 24 | Acknowledged control sequence (u32) |
| 28 | Per-player token (u64) |

HELLO(1): nonce u64, key length u8, key bytes (maximum 64).
WELCOME(2): assigned slot u8 (0–3), echoed nonce u64. Its header supplies the
session/token. Later packets must match session, token and source endpoint.
A repeated HELLO is idempotent. New participants join only in the lobby.

COMMAND(3): command u8 followed by its payload:

| Command | Payload |
|---|---|
| SELECT(1) | car u8: Blue Falcon=0, Wild Goose=1, Golden Fox=2, Fire Stingray=3 |
| LOADED(2) | x u16, y u16, orientation u8 |
| ARM(3) | exact server start time u64 |
| FINISH(4) | none |
| NEXT(5) | none; host only |
| CONFIG(6) | expected players u8 (2–4), track u8 (0–4), league u8 (0–2); host only |
| LEAVE(7) | none |

One command is in flight per client, retried every 250 ms. The server requires
exactly the next sequence, and acknowledges duplicate/expired commands without
reapplying them. A rejected well-sequenced command is acknowledged so an obsolete
ARM cannot block the queue. ERROR(8) contains command and reason bytes.

STATE(4): x u16, y u16, orientation u8, with an independent sequence. Older or
duplicate movement is ignored. Finished/disconnected players cannot be resurrected
by delayed movement. Race generations reject packets from previous races.

SNAPSHOT(5), 88-byte payload: phase, expected players, host, track, league and
armed bitmask (one byte each), two reserved bytes; server time u64 at offset 8;
start time u64 at offset 16. Four 16-byte player records start at offset 24:
status u8, car u8, x u16, y u16, orientation u8, reserved u8, movement sequence
u32, update age milliseconds u32. Empty/finished/disconnected cars are hidden.

Client PING(6) carries its timestamp u64; server PONG(7) returns that timestamp
and server time, both u64. The client estimates RTT/jitter and clock offset.
Server PING carries u64; client PONG echoes u64 for independent server RTT logs.
Heartbeats run in menus and results too. Five seconds of silence disconnects a
joined participant; an unsuccessful join times out after ten seconds.

All selected → loading; all loaded → a start deadline two seconds ahead. Clients
acknowledge that exact deadline. If acknowledgments remain missing within 500 ms
of it, the server schedules another deadline. Clients release only after seeing
all acknowledgments and reaching server time, or seeing server racing state.
All remaining racers finished → results. Host NEXT increments the generation,
restores original ROM bytes and resets every emulator into the next lobby.

## Memory adapter and patches

Ares debugger-memory nodes expose CPU WRAM and cartridge ROM. The adapter verifies
128 KiB WRAM, 512 KiB ROM and SHA-256
`bf16c3c867c58e2ab061c70de9295b6930d63f29f81cc986f5ecae03e0ad18d2`
before permitting writes. ROM offsets below are file offsets after header removal.
The on-disk ROM is never modified. Each overwritten byte is saved and restored
on detach, disconnect or the next race. Game hooks run at a CPU instruction boundary at vertical blank (scanline 225),
not at the PPU frame return, which may suspend the CPU inside an instruction.

| ROM offsets | Purpose |
|---|---|
| 18176, 18143 | Restrict menu mode and disable attract demo |
| 1851F | Hold/release car-selection transition |
| 5486, 532B | Disable automatic opponent-car selection/overwrite |
| 52EF, 572B | Player-ID start position and palette selector |
| 5DFC, 5DDA | Disable generic opponents/catchup and AI movement |
| D3F, 48FF, 4D84, DB6 | Starting boost, pause, results hold, finish palette |
| 3DB2, 3DD5, 3DEF, 3DFE | Prevent collision writes to remote cars |
| 3DAE, 3DCB, 3DE5 | Use local rebound speed |
| 18795, 187FE | Automatically confirm league/class through their normal handlers |
| AB1–AB6 | Hold/release race initialization |

WRAM 54/55/56 identifies the game phase. Menu 5A converts car indices 1 and 2.
Track/league are written to 53/5A. Opponent car types are 1133/1135/1137;
palettes C41/C43/C45/C47. Local x/y are B70/B90 (u16), orientation BD1 (u8).
Remote slots advance by two bytes; the fifth car B78/B98 is hidden each frame.
For assigned player IDs 0–3, the local-slot-to-network-ID tables are respectively
`[0,3,2,1]`, `[1,3,2,0]`, `[2,3,1,0]`, `[3,2,1,0]`.

The original class bypass at 187E6 is deliberately **not** ported: skipping its
normal confirmation handler causes Mode 7 track corruption in Ares. The new branch
at 187FE preserves the game's initialization. P1's remote palette table also
corrects the legacy swap of P2/P4 colors.

The original commented source is
`original/snes9x-1.42-src/snes9x-1.42-src/wsnes9x.cpp`; the author's explanation is
[Tech stuff](https://fzerovs.blogspot.com/2008/07/tech-stuff.html?m=0).

## Validation and scope

`make test` covers malformed/truncated packets, sequence wrap, host permissions,
timeouts, stale movement, repeated races and four-client memory state transitions.
The actual C++ client is tested through deterministic dropped commands/snapshots,
jitter and delayed duplicate movement, with ASan/UBSan enabled. These synthetic
memory tests verify writes and synchronization; they do not verify game rendering.
Use real desktop screenshots and manual play for that separate acceptance check.

The initial implementation targets macOS/Linux POSIX sockets. It has no Windows
socket adapter, encryption, NAT traversal, interpolation, rollback, authoritative
lap validation or compatibility with the legacy protocol. Room keys are private
room admission, not protection against an observer of unencrypted traffic.

### Verified desktop runs (2026-09-08)

Two- and four-process Ares runs reached synchronized racing. Four distinct cars
rendered cleanly on Mute City I and Fire Field after replacing the class bypass;
all four Fire Field captures were visually inspected. The final 35-second run
completed normally with separate settings/screenshots and approximately 60 fps.
Its logs/captures are in `.local-tests/20260908-102939-b7y_z34f/` on the development
machine (generated and ignored). Longer manual five-lap races, gamepad hardware,
all fifteen tracks and real internet conditions remain separate acceptance work.

## Hosted-room extensions (Milestone 2)

Existing gameplay packet layouts and protocol version 1 remain unchanged. Two
new packet types are appended; older clients ignore them and keep their timeout
fallback:

- `JOIN_REJECT` (9): payload is the HELLO nonce (`u64`) and reason (`u8`):
  1 password, 2 full, 3 race underway, 4 owner starting, 5 protocol incompatible.
  The client accepts this only before joining and only for its current nonce.
- `ROOM_CLOSED` (10): one-byte reason (1 room ended), with the recipient's session
  and token in the normal header. The client verifies these before restoring
  patches and disconnecting. A missing notice uses the existing heartbeat timeout.

Hosted servers reserve slot 0 using a random owner nonce, admit no guests before
that owner registers, and end the room when P1 departs. Dedicated servers set no
owner nonce and preserve their previous host reassignment behavior.

Discovery uses a separate IPv4 UDP socket at `239.255.70.90:12001`, multicast TTL
1 and reusable listener sockets. Game port 12001 is reserved. Browsers explicitly
bind an ephemeral query socket, query every two seconds and expire entries after
six seconds. Queries are 16 bytes; replies are unicast, 96 bytes. Multi-byte values
are little-endian:

| Offset | Query / reply |
| --- | --- |
| 0–3 | `FZVD` |
| 4 | Discovery version 1 |
| 5 | Type 1 query / 2 reply |
| 6 | Gameplay protocol version |
| 7 | Reserved zero |
| 8–15 | Query nonce, echoed by replies |
| 16–23 | Reply: server session ID |
| 24–25 | Reply: game port |
| 26–31 | Reply: phase, occupancy, expected players, track, league, password-required |
| 32–95 | Reply: room name, at most 63 printable ASCII bytes plus zero padding |

No password is advertised. Browsers key rooms by session ID and connect using the
reply's source IPv4 address plus its game port; they do not trust a payload-supplied
IP. Invalid lengths, versions and out-of-range metadata are ignored. At most 64
rooms are retained. Listener/discovery failure does not disable direct joining.
