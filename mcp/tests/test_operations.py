from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from ares_mcp.client import AresCommandError, AresDebugClient
from ares_mcp.service import AresService

from fake_bridge import FakeAresBridge


class OperationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.socket_path = Path(self.temp.name) / "ares.sock"
        self.bridge = FakeAresBridge(self.socket_path)
        await self.bridge.start()
        self.client = AresDebugClient(str(self.socket_path), timeout=0.5)
        self.service = AresService(self.client)

    async def asyncTearDown(self) -> None:
        await self.client.close()
        await self.bridge.stop()
        self.temp.cleanup()

    async def test_memory_read_and_write_report_original_values(self) -> None:
        result = await self.service.memory_write("wram", 0x1234, "01FF2300")
        self.assertEqual(result["written"], 4)
        self.assertEqual(result["original"], "00000000")
        read = await self.service.memory_read("wram", 0x1234, 4)
        self.assertEqual(read["data"], "01FF2300")

    async def test_vram_is_available_as_a_read_only_domain(self) -> None:
        self.bridge.vram[0x2468:0x246C] = b"\x12\x34\x56\x78"
        read = await self.service.memory_read("vram", 0x2468, 4)
        self.assertEqual(read["data"], "12345678")
        with self.assertRaises(ValueError):
            await self.service.memory_write("vram", 0x2468, "00")

    async def test_ppu_auxiliary_memories_are_read_only_domains(self) -> None:
        self.bridge.oam[4:8] = b"\x01\x02\x03\x04"
        self.bridge.cgram[8:12] = b"\x05\x06\x07\x08"
        self.assertEqual((await self.service.memory_read("oam", 4, 4))["data"], "01020304")
        self.assertEqual((await self.service.memory_read("cgram", 8, 4))["data"], "05060708")

    async def test_rom_patch_validation_rejection_and_rollback(self) -> None:
        identity = self.bridge.rom_sha256
        with self.assertRaises(AresCommandError) as caught:
            await self.service.rom_patch(4, "FFFF", "EAEA", identity)
        self.assertEqual(caught.exception.code, "EXPECTED_MISMATCH")

        patch = await self.service.rom_patch(4, "0405", "EAEA", identity)
        self.assertEqual(bytes(self.bridge.rom[4:6]), b"\xea\xea")
        listing = await self.client.request("rom.patch.list")
        self.assertEqual(listing["patches"][0]["patch_id"], patch["patch_id"])
        reverted = await self.service.rom_patch_revert(patch["patch_id"], identity)
        self.assertTrue(reverted["reverted"])
        self.assertEqual(bytes(self.bridge.rom[4:6]), b"\x04\x05")

    async def test_rom_hash_mismatch_is_rejected_before_patch(self) -> None:
        with self.assertRaises(AresCommandError) as caught:
            await self.service.rom_patch(0, "0001", "EAEA", "0" * 64)
        self.assertEqual(caught.exception.code, "ROM_HASH_MISMATCH")
        self.assertFalse(self.bridge.patches)

    async def test_memory_snapshot_diff_returns_only_changes(self) -> None:
        snapshot = await self.service.memory_snapshot(
            "wram", 0x120, 32, "menu-before"
        )
        await self.service.memory_write("wram", 0x128, "01")
        await self.service.memory_write("wram", 0x134, "04")
        diff = await self.service.memory_diff(snapshot["snapshot_id"])
        self.assertEqual(diff["change_count"], 2)
        self.assertEqual(
            [change["display_address"] for change in diff["changes"]],
            ["$7E:0128", "$7E:0134"],
        )


if __name__ == "__main__":
    unittest.main()
