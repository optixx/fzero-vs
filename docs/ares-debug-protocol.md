# ares Local Debug Protocol

Version 1, implemented by the F-Zero VS patched ares v148 build.

## Transport and security

The bridge is opt-in. Start ares with:

```sh
./build/bin/ares --ares-debug-socket /tmp/ares-debug.sock game.sfc
```

The argument names an `AF_UNIX` stream socket. ares refuses non-socket files at
that path, refuses to replace a socket with a live listener, removes stale socket
nodes, and sets the new socket to mode `0600`. It never binds a network address.
The current implementation accepts one local client at a time and allows that
client to reconnect without restarting ares.

Messages are UTF-8 JSON objects delimited by one LF byte (NDJSON). A CR directly
before LF is tolerated. Requests are limited to 256 KiB. A request has exactly
one response and there are no unsolicited events in protocol version 1.

## Envelope

Every valid request has a unique, non-negative, JSON-safe integer `id` and a
non-empty string `command`:

```json
{"id":17,"command":"memory.read","domain":"wram","address":4660,"size":32}
```

Success echoes the ID and sets `ok`:

```json
{"id":17,"ok":true,"domain":"wram","address":4660,"size":32,"data":"00112233"}
```

Failure also echoes the ID and carries a stable code plus a human-readable
message:

```json
{"id":17,"ok":false,"error":{"code":"INVALID_ADDRESS","message":"requested region is outside the memory domain"}}
```

Framing/parse errors for which no trustworthy ID exists return `"id":null`.
The current major protocol number is `1`; `emulator.status` always reports it as
`protocol_version`. Clients must reject a different major version.

## Synchronization guarantee

The socket thread never reads or mutates emulator state. It only receives one
line, enqueues it, waits on that command, and writes the completed response.

When the SNES is executing, the enabled bridge adds one disabled-fast-path branch
at the beginning of `SuperFamicom::CPU::main()`. If and only if a command is
pending, the CPU coroutine yields before beginning the next WDC 65C816
instruction. The desktop emulation worker then drains the queue. This distinction
is necessary because the ordinary PPU frame exit can suspend the CPU halfway
through an instruction.

If a bridge pause left the CPU at that boundary, later commands are drained there
without advancing it. If the UI paused or defocused ares at an ordinary frame
exit, a pending command runs only far enough to finish the current instruction
and reach the explicit debug boundary. GDB-halted execution is already at a
debugger-safe point. With no loaded game, non-machine commands are handled in the
worker loop. Read-only operations use the same synchronization as writes.

## Address and data conventions

- All numeric JSON addresses are unsigned decimal integers on the wire. Clients
  may accept user-facing `0x...` syntax before encoding JSON.
- Byte strings are compact, even-length hexadecimal. Responses use uppercase.
- One memory/ROM transfer is limited to 65,536 bytes.
- `wram` addresses are offsets `0x00000..0x1ffff` in the 128 KiB SNES work RAM.
- `vram` addresses are byte offsets `0x0000..0xffff` in the 64 KiB SNES PPU VRAM.
- `oam` and `cgram` expose the PPU's 544-byte sprite-attribute memory and
  512-byte color palette RAM as byte offsets.
- `rom` addresses and every `rom.*` `offset` are offsets into the loaded program
  ROM backing store. They are not SNES CPU addresses.
- `bus` addresses are 24-bit WDC 65C816/SNES CPU addresses
  `0x000000..0xffffff`. Mapped I/O reads can have device semantics; use `wram`
  or `rom` for side-effect-free access to those stores.

Future domains may include `vram`, `cgram`, `oam`, and `sram`. Version 1 rejects
unknown domains.

## Commands

### `emulator.status`

No arguments. Available even with no game loaded. Returns:

- `protocol_version` and `bridge_version`
- `game_loaded`
- `system`, `rom_name`, and immutable base `rom_sha256`, or `null`
- `running`, `paused`, and `state` (`empty`, `running`, or `paused`)
- `frame`, the number of completed video frames since the current game loaded

`rom_sha256` is captured from the program ROM immediately after load, before
debug-bridge runtime patches. Runtime patches therefore do not change identity.

### `emulator.pause`

No arguments. Pauses at the debug instruction boundary. Returns `paused: true`.
This can intentionally override the F-Zero VS frontend's normal refusal to pause
an active multiplayer session.

### `emulator.resume`

No arguments. Idempotently clears the bridge/UI pause. Returns `paused: false`.

### `emulator.reset`

No arguments. Calls the loaded system's normal `power(true)` reset path. Runtime
ROM patches and named in-memory states remain tracked. Returns `reset: true`.

### `cpu.registers`

Initial system: Super Famicom. Returns numeric `pc`, `pb`, `db`, `a`, `x`, `y`,
`s`, `d`, `p`, and boolean `e`; `pc_formatted` contains `$PB:PC`. `flags` decodes
`n`, `v`, `m`, `x`, `d`, `i`, `z`, and `c` as booleans.

### `cpu.disassemble`

Arguments:

```json
{"id":2,"command":"cpu.disassemble","address":8435328,"count":16}
```

`address` is a 24-bit SNES CPU address and `count` is 1..256. The response
includes the E/M/X mode used and structured `instructions` containing numeric
`address`, spaced raw `bytes`, and `instruction`. It delegates decoding and
instruction length to ares's existing WDC65816 disassembler.

All instructions in one request use the CPU's current live E/M/X mode. This is
correct around the current PC. Disassembly of arbitrary or historical code can
be ambiguous when its expected mode differs, and version 1 does not accept an
explicit historical mode. The bridge reports the mode rather than silently
claiming context it does not have.

### `memory.read`

Arguments: `domain`, `address`, `size`. Domains are `wram`, `vram`, `oam`,
`cgram`, `rom`, and `bus`.
Returns the same domain/address, byte `size`, and hex `data`.

### `memory.write`

Arguments: `domain`, `address`, `data`. Version 1 intentionally supports only
`wram`. ROM must use validated `rom.patch`; bus writes are not enabled. Returns
`written`, `original`, and `resulting` in addition to domain/address.

### `rom.read`

Arguments: raw-ROM `offset` and `size`. Returns `offset`, `size`, and `data`.
This is an explicit alias for the ROM backing store, kept separate to make it
harder to confuse a file offset with a mapped SNES CPU address.

### `rom.patch`

Arguments:

```json
{
  "id":8,
  "command":"rom.patch",
  "offset":74291,
  "expected":"D00D",
  "replacement":"EAEA",
  "expected_rom_sha256":"bf16c3c867c58e2ab061c70de9295b6930d63f29f81cc986f5ecae03e0ad18d2"
}
```

`expected` and `replacement` are mandatory, non-empty, same-length strings.
The base ROM hash and current bytes must both match. Active bridge patches cannot
overlap. Success returns `patch_id`, `offset`, `original`, `replacement`, and
`rom_sha256`. Changes affect only the loaded in-memory ROM.

### `rom.patch.list`

No arguments. Returns `patches`, an array of the same patch records. Patch IDs
are process-local opaque strings.

### `rom.patch.revert`

Arguments: `patch_id` and `expected_rom_sha256`. The bridge first verifies ROM
identity and then verifies that the live range still equals the patch's recorded
replacement. If another subsystem changed it, revert fails with `PATCH_CONFLICT`
rather than overwriting that change. Success restores the original bytes and
returns the patch record plus `reverted: true`.

F-Zero VS's pre-existing network patches use the same ROM backing store but have
their own lifecycle. Expected-byte, overlap, and revert-conflict validation make
interactions fail visibly; project-specific coordination remains outside this
generic ares API.

### `savestate.create`

Optional argument: `name` (at most 80 characters). Creates an in-memory state
using the system serializer and returns `state_id`, `name`, serialized `size`,
and `rom_sha256`.

### `savestate.restore`

Argument: `state_id`. Rejects a state captured for a different base ROM. Success
returns `restored: true`. Runtime ROM is deliberately outside SNES save-state
serialization, so restoring a state does not apply or revert runtime ROM patches.

### `savestate.delete`

Argument: `state_id`. Deletes state storage and returns `deleted: true`.

### `screen.capture`

No arguments. Encodes the last completed emulator framebuffer as PNG in
`/tmp/ares-debug-captures/`, a bridge-controlled directory, and returns its
absolute `path`, `width`, `height`, and `frame`. Arbitrary output paths are not
accepted. The MCP sidecar reads this PNG and emits an MCP image content block.

## Error codes

Codes currently used are:

| Code | Meaning |
| --- | --- |
| `MALFORMED_JSON` | Input is not one JSON object. |
| `REQUEST_TOO_LARGE` | An NDJSON request exceeded 256 KiB. |
| `INVALID_REQUEST_ID` | ID is missing, negative, non-integer, or not JSON-safe. |
| `INVALID_COMMAND` / `UNKNOWN_COMMAND` | Command is missing or unsupported. |
| `INVALID_ARGUMENT` | A required field, hex string, or bound is invalid. |
| `INVALID_DOMAIN` / `UNSUPPORTED_DOMAIN` | Domain is unknown or not writable. |
| `INVALID_ADDRESS` | A region exceeds its memory backing store. |
| `NO_GAME` / `UNSUPPORTED_SYSTEM` | The requested machine state is unavailable. |
| `ROM_HASH_MISMATCH` | Base ROM identity differs from the required digest. |
| `EXPECTED_MISMATCH` | Live ROM bytes differ from patch expectations. |
| `PATCH_OVERLAP` / `PATCH_NOT_FOUND` / `PATCH_CONFLICT` | Patch lifecycle validation failed. |
| `SAVESTATE_NOT_FOUND` / `SAVESTATE_FAILED` | State lifecycle or serialization failed. |
| `NO_FRAME` / `CAPTURE_FAILED` | No framebuffer exists or PNG encoding failed. |
| `SERVER_STOPPING` | ares shut down with a queued request. |

Clients should use `code` for control flow and show `message` to a human.

## Iteration-two hook design

- Execute breakpoints belong at the start of `SuperFamicom::CPU::main()`, next
  to the current pending-command boundary, before `debugger.instruction()` and
  opcode fetch.
- CPU read/write watchpoints belong in `SuperFamicom::CPU::read()` and `write()`;
  DMA also accesses the bus through channel paths and must report the initiating
  CPU/DMA context deliberately. Hooks should be guarded by one false fast-path
  flag, with indexed breakpoint/watchpoint lookup only when enabled.
- A stop record should be captured before yielding and include PC, decoded
  instruction, registers, address, old/new value, access type, and frame.
- Trace records should use a bounded ring buffer drained by `trace.read`, never
  unbounded per-access logging or unsolicited socket messages.

These additions fit the same command envelope (`breakpoint.add`,
`watchpoint.write`, `trace.start`, and so on) without changing framing.

## Assembler and symbols direction

Assembler integration is deferred. The recommended first candidate is
[Asar](https://github.com/RPGHacker/asar): it is purpose-built for SNES ROM
patches, supports 65C816, and publishes a C API plus Python DLL bindings. A safe
sidecar integration should assemble against a temporary copy/dummy image,
extract the emitted bytes, and still apply them through `rom.patch` with base
SHA-256 and expected-byte validation. It must not give an assembler arbitrary
access to the live user ROM. `ca65` and `wla-65816` are viable deterministic
fallbacks, but their object/linker workflows add more machinery for small
interactive snippets.

Symbols remain sidecar/project data. A future `symbols/fzero.yaml` can decorate
the structured disassembly response without adding game-specific knowledge to
ares. Structured patch manifests should likewise be validated and decomposed by
Python into the generic `rom.read` / `rom.patch` / `rom.patch.revert` primitives.
