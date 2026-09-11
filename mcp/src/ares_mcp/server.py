"""stdio MCP server exposing the ares debug bridge."""

from __future__ import annotations

import argparse
import json
import logging
import os

from mcp.server import MCPServer

from .client import AresDebugClient
from .protocol import DEFAULT_SOCKET_PATH, DEFAULT_TIMEOUT_SECONDS
from .service import AresService
from .tools import register_all


LOG = logging.getLogger(__name__)


class JsonLogFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        payload = {
            "component": "fzero-dev-mcp",
            "level": record.levelname.lower(),
            "logger": record.name,
            "message": record.getMessage(),
        }
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def create_server(
    socket_path: str = DEFAULT_SOCKET_PATH,
    timeout: float = DEFAULT_TIMEOUT_SECONDS,
) -> MCPServer:
    client = AresDebugClient(socket_path=socket_path, timeout=timeout)
    service = AresService(client)
    server = MCPServer(
        "fzero-dev-mcp",
        title="ares Local Debugger",
        description="Local MCP adapter for the generic ares debug bridge",
        version="0.1.0",
        instructions=(
            "Inspect status and ROM SHA-256 before ROM-specific work. Pause for "
            "coordinated live-state experiments. ROM offsets are not SNES bus "
            "addresses. Always supply the exact loaded ROM SHA-256 when patching."
        ),
    )
    register_all(server, service)
    return server


mcp = create_server(os.environ.get("ARES_DEBUG_SOCKET", DEFAULT_SOCKET_PATH))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--socket",
        default=os.environ.get("ARES_DEBUG_SOCKET", DEFAULT_SOCKET_PATH),
        help="ares Unix debug socket (default: %(default)s)",
    )
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
        help="bridge request timeout in seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--log-level", default="INFO", choices=("DEBUG", "INFO", "WARNING", "ERROR")
    )
    args = parser.parse_args()
    handler = logging.StreamHandler()
    handler.setFormatter(JsonLogFormatter())
    logging.basicConfig(
        level=getattr(logging, args.log_level), handlers=[handler], force=True
    )
    LOG.info("starting stdio MCP server for ares socket %s", args.socket)
    create_server(args.socket, args.timeout).run()


if __name__ == "__main__":
    main()
