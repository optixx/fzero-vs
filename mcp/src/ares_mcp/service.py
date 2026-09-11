"""Higher-level, MCP-independent operations composed from bridge primitives."""

from __future__ import annotations

from typing import Any

from .client import AresCommandError, AresDebugClient
from .models import MemorySnapshot, display_address
from .protocol import normalized_hex, normalized_sha256, validate_region


class AresService:
    def __init__(self, client: AresDebugClient) -> None:
        self.client = client
        self._snapshots: dict[str, MemorySnapshot] = {}
        self._snapshot_names: dict[str, str] = {}
        self._next_snapshot_id = 1

    async def status(self) -> dict[str, Any]:
        return await self.client.request("emulator.status")

    async def require_rom(self, expected_rom_sha256: str) -> str:
        expected = normalized_sha256(expected_rom_sha256)
        status = await self.status()
        if not status.get("game_loaded"):
            raise AresCommandError("NO_GAME", "no game is loaded")
        actual = status.get("rom_sha256")
        if actual != expected:
            raise AresCommandError(
                "ROM_HASH_MISMATCH",
                f"loaded ROM SHA-256 is {actual!r}, not {expected!r}",
            )
        return expected

    async def memory_read(self, domain: str, address: int, size: int) -> dict[str, Any]:
        validate_region(domain, address, size)
        return await self.client.request(
            "memory.read", domain=domain, address=address, size=size
        )

    async def memory_write(self, domain: str, address: int, data: str) -> dict[str, Any]:
        normalized = normalized_hex(data, field="data")
        validate_region(domain, address, len(normalized) // 2)
        if domain != "wram":
            raise ValueError(
                'memory writes currently support only "wram"; use ares_rom_patch for ROM'
            )
        return await self.client.request(
            "memory.write",
            retryable=False,
            domain=domain,
            address=address,
            data=normalized,
        )

    async def rom_patch(
        self,
        offset: int,
        expected: str,
        replacement: str,
        expected_rom_sha256: str,
    ) -> dict[str, Any]:
        expected_data = normalized_hex(expected, field="expected")
        replacement_data = normalized_hex(replacement, field="replacement")
        if len(expected_data) != len(replacement_data):
            raise ValueError("expected and replacement must have the same byte length")
        identity = await self.require_rom(expected_rom_sha256)
        return await self.client.request(
            "rom.patch",
            retryable=False,
            offset=offset,
            expected=expected_data,
            replacement=replacement_data,
            expected_rom_sha256=identity,
        )

    async def rom_patch_revert(
        self, patch_id: str, expected_rom_sha256: str
    ) -> dict[str, Any]:
        identity = await self.require_rom(expected_rom_sha256)
        return await self.client.request(
            "rom.patch.revert",
            retryable=False,
            patch_id=patch_id,
            expected_rom_sha256=identity,
        )

    async def memory_snapshot(
        self,
        domain: str,
        address: int,
        size: int,
        name: str | None = None,
    ) -> dict[str, Any]:
        if name is not None:
            name = name.strip()
            if not name or len(name) > 80:
                raise ValueError("snapshot name must contain 1 to 80 characters")
        response = await self.memory_read(domain, address, size)
        status = await self.status()
        identity = str(status.get("rom_sha256") or "")
        snapshot_id = f"snapshot-{self._next_snapshot_id}"
        self._next_snapshot_id += 1
        snapshot = MemorySnapshot(
            snapshot_id=snapshot_id,
            name=name,
            domain=domain,
            address=address,
            data=bytes.fromhex(str(response["data"])),
            rom_sha256=identity,
        )
        self._snapshots[snapshot_id] = snapshot
        if name is not None:
            self._snapshot_names[name] = snapshot_id
        return {
            "snapshot_id": snapshot_id,
            "name": name,
            "domain": domain,
            "address": address,
            "size": len(snapshot.data),
            "rom_sha256": identity,
        }

    async def memory_diff(
        self, snapshot_id: str, max_changes: int = 4096
    ) -> dict[str, Any]:
        if not 1 <= max_changes <= 65536:
            raise ValueError("max_changes must be between 1 and 65536")
        resolved = self._snapshot_names.get(snapshot_id, snapshot_id)
        snapshot = self._snapshots.get(resolved)
        if snapshot is None:
            raise ValueError(f"unknown memory snapshot: {snapshot_id}")
        status = await self.status()
        identity = str(status.get("rom_sha256") or "")
        if identity != snapshot.rom_sha256:
            raise AresCommandError(
                "ROM_HASH_MISMATCH",
                "the loaded ROM differs from the ROM used for this snapshot",
            )
        response = await self.memory_read(
            snapshot.domain, snapshot.address, len(snapshot.data)
        )
        current = bytes.fromhex(str(response["data"]))
        all_changes = [
            (index, before, current[index])
            for index, before in enumerate(snapshot.data)
            if before != current[index]
        ]
        changes = [
            {
                "address": snapshot.address + index,
                "display_address": display_address(
                    snapshot.domain, snapshot.address + index
                ),
                "before": f"{before:02X}",
                "after": f"{after:02X}",
            }
            for index, before, after in all_changes[:max_changes]
        ]
        return {
            "snapshot_id": snapshot.snapshot_id,
            "name": snapshot.name,
            "domain": snapshot.domain,
            "address": snapshot.address,
            "size": len(snapshot.data),
            "change_count": len(all_changes),
            "returned_change_count": len(changes),
            "truncated": len(all_changes) > len(changes),
            "changes": changes,
        }
