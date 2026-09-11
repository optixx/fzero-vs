from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from mcp.server.mcpserver.utilities.types import Image

from ..client import AresDebugError
from ..protocol import MAX_SCREENSHOT_BYTES
from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_screenshot() -> list[str | Image]:
        """Capture and return the most recently completed ares video frame.

        ares writes a PNG under its controlled temporary capture directory. The
        sidecar reads that file and returns both concise JSON metadata and an MCP
        image content block that a vision-capable agent can inspect directly.
        """
        response = await service.client.request(
            "screen.capture", retryable=False
        )
        path = Path(str(response.get("path", "")))
        capture_root = Path("/tmp/ares-debug-captures").resolve()
        resolved = path.resolve(strict=True)
        if (
            not path.is_absolute()
            or resolved.parent != capture_root
            or resolved.suffix.lower() != ".png"
        ):
            raise AresDebugError("ares returned an invalid screenshot path")
        data = resolved.read_bytes()
        if not data or len(data) > MAX_SCREENSHOT_BYTES:
            raise AresDebugError(
                "ares screenshot is empty or exceeds the 16 MiB limit"
            )
        metadata = {
            "path": str(resolved),
            "width": response.get("width"),
            "height": response.get("height"),
            "frame": response.get("frame"),
        }
        return [json.dumps(metadata, separators=(",", ":")), Image(data=data, format="png")]
