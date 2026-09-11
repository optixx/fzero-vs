from __future__ import annotations

from typing import Any

from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_memory_read(
        domain: str, address: int, size: int
    ) -> dict[str, Any]:
        """Read up to 64 KiB from a named live emulator memory domain.

        Domains: `wram` uses a 0..0x1FFFF work-RAM offset; `vram` uses a
        0..0xFFFF PPU byte offset; `oam` and `cgram` expose the PPU's sprite
        attributes and palette RAM; `rom` uses a raw loaded-cartridge offset;
        `bus` uses a 24-bit SNES CPU address. The result is compact hexadecimal
        data and repeats the exact domain/address used. Bus reads are synchronized
        but I/O-mapped addresses may have device read semantics; prefer a named
        memory domain when possible.
        """
        return await service.memory_read(domain, address, size)

    @mcp.tool()
    @report_tool_errors
    async def ares_memory_write(
        domain: str, address: int, data: str
    ) -> dict[str, Any]:
        """Write raw bytes to live emulated WRAM at a safe execution boundary.

        This modifies the running game's state. `data` is compact even-length
        hexadecimal. Initial support intentionally permits only domain `wram`.
        Do not use this for cartridge ROM; use `ares_rom_patch`, which validates
        ROM identity and expected bytes and supports rollback.
        """
        return await service.memory_write(domain, address, data)

    @mcp.tool()
    @report_tool_errors
    async def ares_memory_snapshot(
        domain: str, address: int, size: int, name: str | None = None
    ) -> dict[str, Any]:
        """Capture a named-domain memory region inside this MCP sidecar.

        The snapshot is not stored in ares and disappears when the sidecar
        restarts. It is tied to the currently loaded ROM SHA-256 and can later be
        compared with `ares_memory_diff`. Names are optional aliases.
        """
        return await service.memory_snapshot(domain, address, size, name)

    @mcp.tool()
    @report_tool_errors
    async def ares_memory_diff(
        snapshot_id: str, max_changes: int = 4096
    ) -> dict[str, Any]:
        """Compare live memory with a prior sidecar snapshot.

        `snapshot_id` may be the returned ID or its optional name. Only changed
        bytes are returned, with raw and human-readable SNES addresses. Results
        are capped by `max_changes`; `change_count` and `truncated` disclose any
        omitted tail. The loaded ROM identity must still match the snapshot.
        """
        return await service.memory_diff(snapshot_id, max_changes)
