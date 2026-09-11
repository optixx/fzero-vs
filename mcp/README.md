# fzero-dev-mcp

`fzero-dev-mcp` is a standalone Python MCP server for the generic local debug
bridge in the patched ares build. MCP and agent logic do not run inside ares.

```text
MCP-capable agent <-- stdio MCP --> fzero-dev-mcp <-- Unix NDJSON --> ares
```

The sidecar can be stopped, upgraded, and restarted without restarting ares. Its
memory snapshots are process-local; ares-owned runtime patches and in-memory save
states survive a sidecar restart as long as ares itself remains open.

## Requirements and install

- Python 3.10 or newer
- `uv` (recommended) or another Python package installer
- the official MCP Python SDK v2 (`mcp>=2,<3`)
- the SNES-only patched ares build described in the project root

From the repository root:

```sh
make ares
uv sync --project mcp --extra dev
```

The project uses SDK v2's `MCPServer`; v1 `FastMCP` imports are intentionally not
used. See the [official Python SDK](https://github.com/modelcontextprotocol/python-sdk).

## Start ares and the sidecar

Start ares with an opt-in, per-instance socket. Quote ROM paths when needed:

```sh
./build/bin/ares \
  --ares-debug-socket /tmp/ares-debug.sock \
  '/path/to/game.sfc'
```

Then start the stdio MCP server:

```sh
uv run --project mcp fzero-dev-mcp --socket /tmp/ares-debug.sock
```

For an MCP host, configure that command as a stdio server instead of running it
in an ordinary terminal. A generic host configuration looks like:

```json
{
  "mcpServers": {
    "ares": {
      "command": "uv",
      "args": [
        "run",
        "--project",
        "/absolute/path/to/fzero-vs/mcp",
        "fzero-dev-mcp",
        "--socket",
        "/tmp/ares-debug.sock"
      ]
    }
  }
}
```

Use your host's equivalent configuration format. Quiet JSON logs go to stderr so
they do not corrupt the stdio MCP stream. `ARES_DEBUG_SOCKET` may set the default
socket, but an explicit `--socket` is clearer when several local ares instances
run.

For Codex, this repository includes `.codex/config.toml`. Once the repository is
trusted, start a new Codex session from the repository root; Codex starts the
stdio sidecar with `uv` and connects it to `/tmp/fzero-vs-ares-debug.sock`.
Launch the matching emulator instance with `make test-2p` (or
`./scripts/test-2p.sh`). The MCP process may start before ares: its client
connects lazily and reconnects after the emulator is restarted.

For interactive inspection of schemas and results:

```sh
cd mcp
uv run mcp dev src/ares_mcp/server.py
```

Set `ARES_DEBUG_SOCKET` first if ares is not using `/tmp/ares-debug.sock`.

## Test the connection

The first useful tool call is `ares_status`. It should return:

- `protocol_version: 1`
- `game_loaded: true`
- `system: "Super Famicom"`
- the loaded ROM's exact SHA-256
- running/paused state and completed-frame count

The sidecar performs an `emulator.status` handshake whenever it connects and
rejects an incompatible bridge major version. Side-effect-free read requests are
retried once after a broken connection. CPU-bus reads are not replayed because
mapped I/O may have device semantics. Mutations are likewise never replayed
after an ambiguous lost response because they may already have taken effect; the
next tool call reconnects.

Run emulator-independent tests with:

```sh
make test-mcp
```

The fake Unix-socket bridge covers request IDs, reconnection, malformed JSON,
invalid commands, timeouts, memory reads/writes, patch expected-byte rejection,
patch rollback, ROM hash mismatch, and memory snapshot/diff behavior.

## Tool groups

- Lifecycle: `ares_status`, `ares_pause`, `ares_resume`, `ares_reset`
- CPU: `ares_cpu_registers`, `ares_disassemble`
- Memory: `ares_memory_read`, `ares_memory_write`,
  `ares_memory_snapshot`, `ares_memory_diff`
- ROM: `ares_rom_read`, `ares_rom_patch`, `ares_rom_patch_list`,
  `ares_rom_patch_revert`
- State/visual: `ares_savestate_create`, `ares_savestate_restore`,
  `ares_savestate_delete`, `ares_screenshot`

Tool docstrings intentionally repeat destructive effects and address semantics so
an LLM sees the safety boundary in `tools/list`. `ares_screenshot` returns both
JSON metadata and an MCP image block. Expected validation, connection, and ares
command failures are emitted as MCP `is_error` tool results with readable text;
unexpected programming failures remain sanitized by the SDK.

## Safe patch workflow

1. Call `ares_status` and retain the exact `rom_sha256`.
2. Read the raw ROM offset with `ares_rom_read`.
3. Optionally create an in-memory state.
4. Call `ares_rom_patch` with the exact expected bytes, replacement bytes, and
   `expected_rom_sha256`.
5. Resume and inspect state or call `ares_screenshot`.
6. Revert by patch ID with the same ROM SHA-256, or keep the patch recorded.

ROM offsets are backing-image offsets. They are not mapped `PB:PC` addresses.
`ares_disassemble` accepts mapped 24-bit SNES CPU addresses instead.

The complete wire contract and synchronization model are documented in
[`docs/ares-debug-protocol.md`](../docs/ares-debug-protocol.md).
