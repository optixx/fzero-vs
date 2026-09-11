# Ares Debug Bridge: Initial Implementation Plan

## Existing ares facilities

- The desktop frontend owns a dedicated emulation worker and already stops it
  safely for UI operations through `Program::Guard`.
- The patched SNES CPU reports the beginning of VBlank from `CPU::main()`, before
  the next WDC65816 instruction. Unlike the PPU frame exit, this is a stable CPU
  instruction boundary and is already used for F-Zero VS runtime ROM updates.
- `WDC65816` exposes register state and a runtime E/M/X-aware disassembler.
- SNES WRAM (`CPU::wram`), CPU bus mappings, and cartridge ROM
  (`Cartridge::rom`) are distinct stores. The cartridge debugger already uses
  `ReadableMemory::program()` for runtime ROM edits.
- Normal reset is `Node::System::power(true)`. In-memory state can use the
  existing `serialize()` / `unserialize()` callbacks.
- The desktop `Program::video()` callback has the final rendered framebuffer and
  the existing PNG encoder used by manual screenshots.

## Milestone design

1. Add an opt-in Unix-domain socket server to `desktop-ui`, enabled with
   `--ares-debug-socket PATH`. The socket thread only frames NDJSON, queues
   requests, waits for completion, and writes exactly one response.
2. Add a pending-command check at the start of `SuperFamicom::CPU::main()` and
   yield to the worker before the next instruction only when the queue is
   non-empty. Drain the queue after the core has yielded. When already suspended
   at that boundary or no game is loaded, drain it in the worker loop. No
   emulator state is accessed by the IPC thread.
3. Implement protocol version 1 commands for status, pause/resume/reset,
   WDC65816 registers and disassembly, bounded named-domain memory access,
   ROM reads and validated patches, patch rollback, in-memory states, and PNG
   capture from the last completed frame.
4. Cache the unmodified loaded-ROM SHA-256 at load time. Require it for ROM
   patch and revert requests, reject overlapping patches, and refuse rollback
   if another subsystem changed the replacement bytes.
5. Add the Python `fzero-dev-mcp` sidecar using the supported MCP Python SDK v2.
   Keep connection/retry and protocol validation below the tool layer; keep
   snapshots and diffs entirely in Python.
6. Test the Python client and higher-level operations against a fake Unix-socket
   bridge. Export a new ordered ares patch and verify it against the pinned v148
   source before building the SNES-only desktop target.

## Deferred hooks

Breakpoints should test the PC in `SuperFamicom::CPU::main()` immediately before
`debugger.instruction()`; watchpoints should wrap `CPU::read` / `CPU::write` and
DMA access paths. Both should be guarded by one disabled fast-path flag so normal
emulation pays only a predictable branch. Trace buffering, symbols, assembly,
and F-Zero-specific knowledge remain sidecar/project concerns.
