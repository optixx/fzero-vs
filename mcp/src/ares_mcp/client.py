"""Async reconnecting client for the local ares debug socket."""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
from typing import Any

from .protocol import (
    ARES_DEBUG_PROTOCOL_VERSION,
    DEFAULT_SOCKET_PATH,
    DEFAULT_TIMEOUT_SECONDS,
    READ_ONLY_COMMANDS,
)

LOG = logging.getLogger(__name__)
_STREAM_LIMIT = 1024 * 1024


class AresDebugError(RuntimeError):
    """Base error for sidecar-to-ares communication."""


class AresConnectionError(AresDebugError):
    """The local bridge could not be reached or returned invalid framing."""


class AresCommandError(AresDebugError):
    """ares rejected a valid protocol request."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class IncompatibleProtocolError(AresDebugError):
    """The bridge and sidecar use different protocol major versions."""


class AresDebugClient:
    """Maintain one ordered connection and reconnect safely between requests.

    Read-only commands are retried once after a broken connection. Mutations are
    never replayed after an ambiguous connection loss because the first attempt
    may already have taken effect inside ares.
    """

    def __init__(
        self,
        socket_path: str = DEFAULT_SOCKET_PATH,
        timeout: float = DEFAULT_TIMEOUT_SECONDS,
    ) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._ids = itertools.count(1)
        self._lock = asyncio.Lock()
        self._compatible = False

    async def close(self) -> None:
        async with self._lock:
            await self._disconnect_locked()

    async def request(
        self,
        command: str,
        *,
        retryable: bool | None = None,
        **parameters: Any,
    ) -> dict[str, Any]:
        """Send one command and return response fields excluding id/ok."""
        if retryable is None:
            retryable = command in READ_ONLY_COMMANDS and not (
                command == "memory.read" and parameters.get("domain") == "bus"
            )
        async with self._lock:
            attempts = 2 if retryable else 1
            for attempt in range(attempts):
                try:
                    await self._ensure_connected_locked()
                    return await self._exchange_locked(command, parameters)
                except AresCommandError:
                    raise
                except IncompatibleProtocolError:
                    await self._disconnect_locked()
                    raise
                except (
                    OSError,
                    asyncio.IncompleteReadError,
                    asyncio.TimeoutError,
                    json.JSONDecodeError,
                    AresConnectionError,
                ) as error:
                    await self._disconnect_locked()
                    if attempt + 1 < attempts:
                        LOG.info("ares bridge connection lost; reconnecting")
                        continue
                    suffix = (
                        " The mutation may have completed; it was not replayed."
                        if not retryable
                        else ""
                    )
                    raise AresConnectionError(
                        f"ares bridge request failed: {error}.{suffix}"
                    ) from error
        raise AssertionError("unreachable")

    async def _ensure_connected_locked(self) -> None:
        if self._writer is not None and not self._writer.is_closing() and self._compatible:
            return
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_unix_connection(self.socket_path, limit=_STREAM_LIMIT),
                timeout=self.timeout,
            )
        except (OSError, asyncio.TimeoutError) as error:
            raise AresConnectionError(
                f"cannot connect to ares debug socket {self.socket_path}: {error}"
            ) from error
        self._reader, self._writer = reader, writer
        self._compatible = False
        status = await self._exchange_locked("emulator.status", {})
        version = status.get("protocol_version")
        if version != ARES_DEBUG_PROTOCOL_VERSION:
            raise IncompatibleProtocolError(
                f"ares protocol version {version!r} is incompatible with sidecar version "
                f"{ARES_DEBUG_PROTOCOL_VERSION}"
            )
        self._compatible = True
        LOG.info("connected to ares debug bridge at %s", self.socket_path)

    async def _exchange_locked(self, command: str, parameters: dict[str, Any]) -> dict[str, Any]:
        if self._reader is None or self._writer is None:
            raise AresConnectionError("ares bridge is not connected")
        request_id = next(self._ids)
        request = {"id": request_id, "command": command, **parameters}
        wire = (
            json.dumps(request, separators=(",", ":"), ensure_ascii=False).encode(
                "utf-8"
            )
            + b"\n"
        )
        self._writer.write(wire)
        await asyncio.wait_for(self._writer.drain(), timeout=self.timeout)
        line = await asyncio.wait_for(self._reader.readline(), timeout=self.timeout)
        if not line:
            raise asyncio.IncompleteReadError(line, 1)
        if len(line) >= _STREAM_LIMIT and not line.endswith(b"\n"):
            raise AresConnectionError("ares response exceeds the framing limit")
        response = json.loads(line)
        if not isinstance(response, dict):
            raise AresConnectionError("ares response is not a JSON object")
        if response.get("id") != request_id:
            raise AresConnectionError(
                f"ares response id {response.get('id')!r} does not match request id {request_id}"
            )
        if response.get("ok") is not True:
            error = response.get("error")
            if not isinstance(error, dict):
                raise AresConnectionError("ares error response has no structured error")
            raise AresCommandError(
                str(error.get("code", "UNKNOWN_ERROR")),
                str(error.get("message", "ares rejected the request")),
            )
        return {key: value for key, value in response.items() if key not in {"id", "ok"}}

    async def _disconnect_locked(self) -> None:
        writer, self._writer = self._writer, None
        self._reader = None
        self._compatible = False
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
