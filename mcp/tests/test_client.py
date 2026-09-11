from __future__ import annotations

import asyncio
from pathlib import Path
import tempfile
import unittest

from ares_mcp.client import (
    AresCommandError,
    AresConnectionError,
    AresDebugClient,
    IncompatibleProtocolError,
)

from fake_bridge import FakeAresBridge, send_raw


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.socket_path = Path(self.temp.name) / "ares.sock"
        self.bridge = FakeAresBridge(self.socket_path)
        await self.bridge.start()
        self.client = AresDebugClient(str(self.socket_path), timeout=0.25)

    async def asyncTearDown(self) -> None:
        await self.client.close()
        await self.bridge.stop()
        self.temp.cleanup()

    async def test_request_ids_are_unique_and_responses_match(self) -> None:
        await self.client.request("emulator.status")
        await self.client.request("cpu.registers")
        self.assertEqual(len(self.bridge.request_ids), len(set(self.bridge.request_ids)))
        self.assertGreaterEqual(len(self.bridge.request_ids), 3)  # handshake + two calls

    async def test_malformed_json_has_structured_error(self) -> None:
        response = await send_raw(self.socket_path, b'{"id":1,broken')
        self.assertIsNone(response["id"])
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "MALFORMED_JSON")

    async def test_invalid_command_is_not_retried(self) -> None:
        with self.assertRaises(AresCommandError) as caught:
            await self.client.request("does.not.exist", retryable=False)
        self.assertEqual(caught.exception.code, "UNKNOWN_COMMAND")

    async def test_timeout(self) -> None:
        with self.assertRaises(AresConnectionError):
            await self.client.request(
                "test.sleep", retryable=False, seconds=1.0
            )

    async def test_reconnect_after_bridge_restart(self) -> None:
        await self.client.request("emulator.status")
        first_connections = self.bridge.connections
        await self.bridge.stop()
        replacement = FakeAresBridge(self.socket_path)
        await replacement.start()
        self.bridge = replacement
        status = await self.client.request("emulator.status")
        self.assertTrue(status["game_loaded"])
        self.assertGreaterEqual(self.bridge.connections, 1)
        self.assertGreaterEqual(first_connections, 1)

    async def test_bus_reads_are_not_replayed_but_rom_reads_are(self) -> None:
        self.bridge.disconnect_once_for.add("memory.read")
        with self.assertRaises(AresConnectionError):
            await self.client.request("memory.read", domain="bus", address=0, size=1)

        self.bridge.disconnect_once_for.add("memory.read")
        response = await self.client.request(
            "memory.read", domain="rom", address=0, size=1
        )
        self.assertEqual(response["data"], "00")

    async def test_incompatible_protocol_is_rejected(self) -> None:
        await self.client.close()
        await self.bridge.stop()
        self.bridge = FakeAresBridge(self.socket_path, protocol_version=2)
        await self.bridge.start()
        with self.assertRaises(IncompatibleProtocolError):
            await self.client.request("emulator.status")


if __name__ == "__main__":
    unittest.main()
