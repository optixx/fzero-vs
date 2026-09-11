"""State owned by the Python sidecar rather than the emulator bridge."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class MemorySnapshot:
    snapshot_id: str
    name: str | None
    domain: str
    address: int
    data: bytes
    rom_sha256: str


def display_address(domain: str, address: int) -> str:
    if domain == "wram" and address < 128 * 1024:
        return f"${0x7E + address // 0x10000:02X}:{address & 0xFFFF:04X}"
    if domain == "bus":
        return f"${address >> 16:02X}:{address & 0xFFFF:04X}"
    if domain in {"vram", "oam", "cgram"}:
        return f"{domain.upper()}+0x{address:04X}"
    if domain == "rom":
        return f"ROM+0x{address:06X}"
    return f"{domain}+0x{address:X}"
