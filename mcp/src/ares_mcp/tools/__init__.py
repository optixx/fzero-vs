"""MCP tool registration grouped by emulator concern."""

from __future__ import annotations

from typing import Any

from . import cpu, emulator, memory, rom, savestate, screen
from ..service import AresService


def register_all(mcp: Any, service: AresService) -> None:
    emulator.register(mcp, service)
    cpu.register(mcp, service)
    memory.register(mcp, service)
    rom.register(mcp, service)
    savestate.register(mcp, service)
    screen.register(mcp, service)
