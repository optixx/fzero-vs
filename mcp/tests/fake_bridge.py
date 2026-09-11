"""Small in-process ares bridge double used by sidecar tests."""

from __future__ import annotations

import asyncio
import hashlib
import json
from pathlib import Path
from typing import Any


class FakeAresBridge:
    def __init__(self, socket_path: Path, protocol_version: int = 1) -> None:
        self.socket_path = socket_path
        self.protocol_version = protocol_version
        self.wram = bytearray(128 * 1024)
        self.vram = bytearray(64 * 1024)
        self.oam = bytearray(544)
        self.cgram = bytearray(512)
        self.rom = bytearray(range(256)) * 16
        self.rom_sha256 = hashlib.sha256(self.rom).hexdigest()
        self.patches: dict[str, dict[str, Any]] = {}
        self.disconnect_once_for: set[str] = set()
        self.request_ids: list[int] = []
        self.connections = 0
        self._server: asyncio.AbstractServer | None = None
        self._writers: set[asyncio.StreamWriter] = set()
        self._next_patch = 1

    async def start(self) -> None:
        self.socket_path.unlink(missing_ok=True)
        self._server = await asyncio.start_unix_server(
            self._handle, path=str(self.socket_path), limit=1024 * 1024
        )

    async def stop(self) -> None:
        for writer in list(self._writers):
            writer.close()
        await asyncio.gather(
            *(writer.wait_closed() for writer in list(self._writers)),
            return_exceptions=True,
        )
        self._writers.clear()
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        self.socket_path.unlink(missing_ok=True)

    async def _handle(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        self.connections += 1
        self._writers.add(writer)
        try:
            while line := await reader.readline():
                try:
                    request = json.loads(line)
                except json.JSONDecodeError:
                    await self._respond(
                        writer,
                        {
                            "id": None,
                            "ok": False,
                            "error": {
                                "code": "MALFORMED_JSON",
                                "message": "request is not valid JSON",
                            },
                        },
                    )
                    continue
                request_id = request.get("id")
                if isinstance(request_id, int):
                    self.request_ids.append(request_id)
                response = await self._dispatch(request)
                if response is None:
                    writer.close()
                    await writer.wait_closed()
                    return
                await self._respond(writer, response)
        finally:
            self._writers.discard(writer)
            writer.close()

    async def _respond(self, writer: asyncio.StreamWriter, response: dict[str, Any]) -> None:
        writer.write(json.dumps(response, separators=(",", ":")).encode() + b"\n")
        await writer.drain()

    def _ok(self, request: dict[str, Any], **fields: Any) -> dict[str, Any]:
        return {"id": request.get("id"), "ok": True, **fields}

    def _error(
        self, request: dict[str, Any], code: str, message: str
    ) -> dict[str, Any]:
        return {
            "id": request.get("id"),
            "ok": False,
            "error": {"code": code, "message": message},
        }

    async def _dispatch(self, request: dict[str, Any]) -> dict[str, Any] | None:
        command = request.get("command")
        if command in self.disconnect_once_for:
            self.disconnect_once_for.remove(command)
            return None
        if command == "test.disconnect":
            return None
        if command == "test.sleep":
            await asyncio.sleep(float(request.get("seconds", 1)))
            return self._ok(request)
        if command == "emulator.status":
            return self._ok(
                request,
                protocol_version=self.protocol_version,
                bridge_version="fake",
                game_loaded=True,
                system="Super Famicom",
                rom_name="fake.sfc",
                rom_sha256=self.rom_sha256,
                running=True,
                paused=False,
                frame=10,
            )
        if command in {"emulator.pause", "emulator.resume", "emulator.reset"}:
            return self._ok(request, paused=command == "emulator.pause")
        if command == "cpu.registers":
            return self._ok(request, pc=0x8000, pb=0x80, p=0x34, e=False)
        if command == "cpu.disassemble":
            return self._ok(request, instructions=[])
        if command in {"memory.read", "rom.read"}:
            domain = request.get("domain", "rom")
            address = int(request.get("address", request.get("offset", 0)))
            size = int(request.get("size", 0))
            store = {
                "wram": self.wram,
                "vram": self.vram,
                "oam": self.oam,
                "cgram": self.cgram,
                "rom": self.rom,
            }.get(domain)
            if store is None:
                return self._error(request, "INVALID_DOMAIN", "outside memory domain")
            if address < 0 or size < 1 or address + size > len(store):
                return self._error(request, "INVALID_ADDRESS", "outside memory domain")
            fields = {
                "domain": domain,
                "address": address,
                "size": size,
                "data": bytes(store[address : address + size]).hex().upper(),
            }
            if command == "rom.read":
                fields["offset"] = fields.pop("address")
            return self._ok(request, **fields)
        if command == "memory.write":
            if request.get("domain") != "wram":
                return self._error(request, "UNSUPPORTED_DOMAIN", "write only supports wram")
            address = int(request.get("address", 0))
            data = bytes.fromhex(str(request.get("data", "")))
            if address < 0 or address + len(data) > len(self.wram):
                return self._error(request, "INVALID_ADDRESS", "outside WRAM")
            original = bytes(self.wram[address : address + len(data)])
            self.wram[address : address + len(data)] = data
            return self._ok(
                request,
                domain="wram",
                address=address,
                written=len(data),
                original=original.hex().upper(),
                resulting=data.hex().upper(),
            )
        if command == "rom.patch":
            if request.get("expected_rom_sha256") != self.rom_sha256:
                return self._error(request, "ROM_HASH_MISMATCH", "ROM identity differs")
            offset = int(request.get("offset", 0))
            expected = bytes.fromhex(str(request.get("expected", "")))
            replacement = bytes.fromhex(str(request.get("replacement", "")))
            actual = bytes(self.rom[offset : offset + len(expected)])
            if actual != expected:
                return self._error(request, "EXPECTED_MISMATCH", "ROM bytes differ")
            patch_id = f"patch-{self._next_patch}"
            self._next_patch += 1
            self.rom[offset : offset + len(replacement)] = replacement
            self.patches[patch_id] = {
                "patch_id": patch_id,
                "offset": offset,
                "original": expected.hex().upper(),
                "replacement": replacement.hex().upper(),
                "rom_sha256": self.rom_sha256,
            }
            return self._ok(request, **self.patches[patch_id])
        if command == "rom.patch.list":
            return self._ok(request, patches=list(self.patches.values()))
        if command == "rom.patch.revert":
            if request.get("expected_rom_sha256") != self.rom_sha256:
                return self._error(request, "ROM_HASH_MISMATCH", "ROM identity differs")
            patch = self.patches.get(str(request.get("patch_id")))
            if patch is None:
                return self._error(request, "PATCH_NOT_FOUND", "unknown patch")
            offset = patch["offset"]
            replacement = bytes.fromhex(patch["replacement"])
            if bytes(self.rom[offset : offset + len(replacement)]) != replacement:
                return self._error(request, "PATCH_CONFLICT", "replacement was changed")
            self.rom[offset : offset + len(replacement)] = bytes.fromhex(patch["original"])
            del self.patches[patch["patch_id"]]
            return self._ok(request, **patch, reverted=True)
        if command.startswith("savestate."):
            return self._ok(request, state_id=str(request.get("state_id", "state-1")))
        return self._error(request, "UNKNOWN_COMMAND", f"unknown command {command!r}")


async def send_raw(socket_path: Path, line: bytes) -> dict[str, Any]:
    reader, writer = await asyncio.open_unix_connection(str(socket_path))
    writer.write(line + b"\n")
    await writer.drain()
    response = json.loads(await reader.readline())
    writer.close()
    await writer.wait_closed()
    return response
