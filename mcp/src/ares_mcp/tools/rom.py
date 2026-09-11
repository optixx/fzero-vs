from __future__ import annotations

from typing import Any

from ..protocol import validate_region
from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_rom_read(offset: int, size: int) -> dict[str, Any]:
        """Read bytes by raw offset in the currently loaded cartridge ROM image.

        `offset` is a file/backing-store offset, never a SNES CPU-bus address.
        Reads are limited to 64 KiB and return compact hexadecimal data. Use
        `ares_disassemble` for mapped CPU addresses.
        """
        validate_region("rom", offset, size)
        return await service.client.request("rom.read", offset=offset, size=size)

    @mcp.tool()
    @report_tool_errors
    async def ares_rom_patch(
        offset: int,
        expected: str,
        replacement: str,
        expected_rom_sha256: str,
    ) -> dict[str, Any]:
        """Apply a validated, reversible runtime cartridge-ROM patch.

        `offset` is a raw ROM-image offset. `expected` and `replacement` are
        same-length compact hex strings. The patch is rejected unless both the
        immutable loaded-ROM SHA-256 and the current bytes exactly match. Active
        patches may not overlap. Save the returned patch ID for rollback.
        """
        return await service.rom_patch(
            offset, expected, replacement, expected_rom_sha256
        )

    @mcp.tool()
    @report_tool_errors
    async def ares_rom_patch_list() -> dict[str, Any]:
        """List active runtime ROM patches tracked by the ares bridge.

        Each entry includes patch ID, raw ROM offset, original and replacement
        bytes, and the immutable ROM SHA-256. Project-specific patch meaning is
        deliberately not embedded in ares.
        """
        return await service.client.request("rom.patch.list")

    @mcp.tool()
    @report_tool_errors
    async def ares_rom_patch_revert(
        patch_id: str, expected_rom_sha256: str
    ) -> dict[str, Any]:
        """Revert one tracked runtime ROM patch after identity/conflict checks.

        The immutable loaded-ROM SHA-256 must match. Revert is rejected if the
        live bytes no longer equal that patch's replacement, preventing silent
        clobbering of another patching subsystem. A lost response is not replayed.
        """
        return await service.rom_patch_revert(patch_id, expected_rom_sha256)
