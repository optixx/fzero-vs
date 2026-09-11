from __future__ import annotations

from collections.abc import Awaitable, Callable
from functools import wraps
from typing import Any, ParamSpec, TypeVar

from mcp.server.mcpserver.exceptions import ToolError

from ..client import AresDebugError


P = ParamSpec("P")
R = TypeVar("R")


def report_tool_errors(
    function: Callable[P, Awaitable[R]],
) -> Callable[P, Awaitable[R]]:
    """Expose expected operational failures to the calling model in MCP v2."""

    @wraps(function)
    async def wrapped(*args: P.args, **kwargs: P.kwargs) -> R:
        try:
            return await function(*args, **kwargs)
        except (AresDebugError, ValueError, OSError) as error:
            raise ToolError(str(error)) from error

    return wrapped
