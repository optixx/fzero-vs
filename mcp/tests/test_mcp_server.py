from __future__ import annotations

import unittest

try:
    from mcp import Client
except ImportError:
    Client = None


@unittest.skipIf(Client is None, "MCP SDK is not installed")
class MCPServerTests(unittest.IsolatedAsyncioTestCase):
    async def test_registers_the_public_tool_surface(self) -> None:
        from ares_mcp.server import create_server

        expected = {
            "ares_status",
            "ares_pause",
            "ares_resume",
            "ares_reset",
            "ares_cpu_registers",
            "ares_disassemble",
            "ares_memory_read",
            "ares_memory_write",
            "ares_memory_snapshot",
            "ares_memory_diff",
            "ares_rom_read",
            "ares_rom_patch",
            "ares_rom_patch_list",
            "ares_rom_patch_revert",
            "ares_savestate_create",
            "ares_savestate_restore",
            "ares_savestate_delete",
            "ares_screenshot",
        }

        async with Client(create_server("/tmp/does-not-need-to-exist.sock")) as client:
            listing = await client.list_tools()
            result = await client.call_tool("ares_status", {})

        self.assertEqual({tool.name for tool in listing.tools}, expected)
        self.assertTrue(all(tool.description for tool in listing.tools))
        self.assertTrue(result.is_error)
        self.assertIn("cannot connect to ares debug socket", result.content[0].text)


if __name__ == "__main__":
    unittest.main()
