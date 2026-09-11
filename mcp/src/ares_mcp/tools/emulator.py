from __future__ import annotations

from typing import Any

from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_status() -> dict[str, Any]:
        """Inspect the local ares instance without changing it.

        Returns game/system identity, the immutable loaded-ROM SHA-256 used for
        patch safety, running/paused state, completed-frame count, bridge build
        version, and local protocol version. Call this before ROM-specific work.
        """
        return await service.status()

    @mcp.tool()
    @report_tool_errors
    async def ares_pause() -> dict[str, Any]:
        """Pause ares at the next safe SNES CPU instruction boundary.

        Use this before a coordinated set of inspections or live-memory edits.
        The request does not mutate CPU/WRAM state halfway through an instruction.
        """
        return await service.client.request("emulator.pause", retryable=True)

    @mcp.tool()
    @report_tool_errors
    async def ares_resume() -> dict[str, Any]:
        """Resume a paused ares instance.

        The operation is idempotent. Live game execution continues immediately
        after the synchronization boundary at which it was paused.
        """
        return await service.client.request("emulator.resume", retryable=True)

    @mcp.tool()
    @report_tool_errors
    async def ares_reset() -> dict[str, Any]:
        """Reset the loaded emulated machine through ares's normal reset path.

        This is a destructive live-state operation. It does not arbitrarily
        clear memory, does not unload the game, and does not remove runtime ROM
        patches. A lost response is never automatically replayed.
        """
        return await service.client.request("emulator.reset", retryable=False)
