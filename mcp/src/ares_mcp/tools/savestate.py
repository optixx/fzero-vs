from __future__ import annotations

from typing import Any

from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_savestate_create(name: str | None = None) -> dict[str, Any]:
        """Create an in-memory ares save state and return an opaque state ID.

        The state lives only in the ares process, is tied to the loaded ROM, and
        exposes no filesystem path. Runtime ROM patches are separate from core
        save states and are not implicitly reverted when a state is restored.
        """
        if name is not None and (not name.strip() or len(name) > 80):
            raise ValueError("state name must contain 1 to 80 characters")
        return await service.client.request(
            "savestate.create", retryable=False, name=name
        )

    @mcp.tool()
    @report_tool_errors
    async def ares_savestate_restore(state_id: str) -> dict[str, Any]:
        """Restore an in-memory state by opaque ID at a safe emulator boundary.

        This destructively replaces live emulated machine state. It is rejected
        if the current loaded ROM identity differs. A lost response is not
        replayed because the restore may already have occurred.
        """
        return await service.client.request(
            "savestate.restore", retryable=False, state_id=state_id
        )

    @mcp.tool()
    @report_tool_errors
    async def ares_savestate_delete(state_id: str) -> dict[str, Any]:
        """Delete an in-memory state from ares without changing live execution."""
        return await service.client.request(
            "savestate.delete", retryable=False, state_id=state_id
        )
