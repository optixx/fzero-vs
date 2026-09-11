"""Shared constants and validation for the ares NDJSON protocol."""

from __future__ import annotations

import re

ARES_DEBUG_PROTOCOL_VERSION = 1
DEFAULT_SOCKET_PATH = "/tmp/ares-debug.sock"
DEFAULT_TIMEOUT_SECONDS = 5.0
MAX_MEMORY_REQUEST = 64 * 1024
MAX_SCREENSHOT_BYTES = 16 * 1024 * 1024

READ_ONLY_COMMANDS = frozenset(
    {
        "emulator.status",
        "cpu.registers",
        "cpu.disassemble",
        "memory.read",
        "rom.read",
        "rom.patch.list",
    }
)

_HEX_RE = re.compile(r"^[0-9a-fA-F]*$")
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def normalized_hex(value: str, *, field: str, allow_empty: bool = False) -> str:
    """Validate compact even-length hex and return uppercase canonical form."""
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a hexadecimal string")
    if not value and not allow_empty:
        raise ValueError(f"{field} must not be empty")
    if len(value) % 2 or not _HEX_RE.fullmatch(value):
        raise ValueError(f"{field} must contain an even number of hexadecimal digits")
    return value.upper()


def normalized_sha256(value: str) -> str:
    """Validate and normalize a SHA-256 digest."""
    if not isinstance(value, str) or not _SHA256_RE.fullmatch(value):
        raise ValueError("expected_rom_sha256 must be exactly 64 hexadecimal digits")
    return value.lower()


def validate_region(domain: str, address: int, size: int) -> None:
    """Validate the sidecar-wide safe bounds before a request reaches ares."""
    if domain not in {"wram", "vram", "oam", "cgram", "rom", "bus"}:
        raise ValueError("domain must be one of: wram, vram, oam, cgram, rom, bus")
    if not isinstance(address, int) or isinstance(address, bool) or address < 0:
        raise ValueError("address must be a non-negative integer")
    if not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= MAX_MEMORY_REQUEST:
        raise ValueError(f"size must be between 1 and {MAX_MEMORY_REQUEST} bytes")
    limit = {"wram": 128 * 1024, "vram": 64 * 1024, "oam": 544, "cgram": 512, "bus": 1 << 24}.get(domain)
    if limit is not None and (address > limit or size > limit - address):
        raise ValueError(f"requested region is outside the {domain} address space")
