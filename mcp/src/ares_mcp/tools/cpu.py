from __future__ import annotations

from typing import Any

from ..service import AresService
from .common import report_tool_errors


def register(mcp: Any, service: AresService) -> None:
    @mcp.tool()
    @report_tool_errors
    async def ares_cpu_registers() -> dict[str, Any]:
        """Read the live Super Famicom WDC 65C816 register set.

        Returns PC/PB/DB/A/X/Y/S/D/P/E as raw integers plus decoded processor
        flags and formatted bank:address text. The read occurs at a stable CPU
        instruction boundary, or while the emulator is paused.
        """
        return await service.client.request("cpu.registers")

    @mcp.tool()
    @report_tool_errors
    async def ares_disassemble(address: int, count: int = 16) -> dict[str, Any]:
        """Disassemble live SNES CPU-bus code with ares's WDC 65C816 decoder.

        `address` is a 24-bit SNES CPU address such as 0x80B680, not a ROM-file
        offset. `count` is 1..256. Immediate widths use the CPU's current E/M/X
        mode for the whole request; arbitrary historical code may require mode
        knowledge that is not represented by the current live state.
        """
        if (
            not isinstance(address, int)
            or isinstance(address, bool)
            or not 0 <= address <= 0xFFFFFF
        ):
            raise ValueError("address must be a 24-bit SNES CPU address")
        if not isinstance(count, int) or isinstance(count, bool) or not 1 <= count <= 256:
            raise ValueError("count must be between 1 and 256 instructions")
        return await service.client.request(
            "cpu.disassemble", address=address, count=count
        )
