# SPDX-License-Identifier: GPL-3.0-only
"""A client of melodyd's protocol v1: line-delimited JSON over TCP.

Requests carry an id and are answered with it; events come unasked. The
first thing said on a TCP connection is the password (session.authenticate).
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import contextlib
import itertools
import json
import logging
from typing import Any

_LOGGER = logging.getLogger(__name__)

# An answer can be a whole library listing; the engine's own limit on what it
# reads is 1 MiB, on what it says far more.
_LINE_LIMIT = 64 * 1024 * 1024


class MelodyError(Exception):
    """The engine answered with an error, or could not be asked."""

    def __init__(self, message: str, code: str | None = None) -> None:
        """Keep the engine's error code beside its message."""
        super().__init__(message)
        self.code = code


class MelodyConnectionError(MelodyError):
    """The engine could not be reached, or went away."""


class MelodyAuthError(MelodyError):
    """The engine refused the password."""


EventHandler = Callable[[str, dict[str, Any]], None]


class MelodyClient:
    """One connection to one engine."""

    def __init__(
        self,
        host: str,
        port: int,
        password: str | None,
        *,
        on_event: EventHandler | None = None,
        on_disconnect: Callable[[], None] | None = None,
    ) -> None:
        """Remember where the engine is; connect() reaches it."""
        self.host = host
        self.port = port
        self._password = password
        self._on_event = on_event
        self._on_disconnect = on_disconnect
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reading: asyncio.Task[None] | None = None
        self._pending: dict[int, asyncio.Future[Any]] = {}
        self._ids = itertools.count(1)
        self._closing = False

    @property
    def connected(self) -> bool:
        """Whether the connection is up."""
        return self._writer is not None and not self._writer.is_closing()

    async def connect(self, timeout: float = 10.0) -> None:
        """Connect and authenticate; raises MelodyConnectionError or MelodyAuthError."""
        self._closing = False
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port, limit=_LINE_LIMIT), timeout
            )
        except (OSError, TimeoutError) as err:
            raise MelodyConnectionError(f"cannot reach {self.host}:{self.port}: {err}") from err
        self._reading = asyncio.create_task(self._read(), name="melody client reader")
        try:
            answer = await self.call(
                "session.authenticate", {"password": self._password or ""}, timeout=timeout
            )
        except MelodyError as err:
            await self.close()
            if err.code in ("unauthorized", "forbidden", "permission_denied"):
                raise MelodyAuthError(str(err), err.code) from err
            raise
        if not answer.get("authenticated"):
            await self.close()
            raise MelodyAuthError("the engine did not take the password")

    async def close(self) -> None:
        """Hang up; what was asked and not answered fails."""
        self._closing = True
        if self._writer is not None:
            self._writer.close()
            with contextlib.suppress(OSError, ConnectionError):
                await self._writer.wait_closed()
        if self._reading is not None and self._reading is not asyncio.current_task():
            self._reading.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._reading
        self._fail_pending(MelodyConnectionError("the connection was closed"))
        self._writer = None
        self._reader = None
        self._reading = None

    async def call(
        self, method: str, params: dict[str, Any] | None = None, timeout: float = 15.0
    ) -> Any:
        """Ask and wait for the answer's result; raises MelodyError."""
        if self._writer is None or self._writer.is_closing():
            raise MelodyConnectionError("not connected")
        request_id = next(self._ids)
        future: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        line = json.dumps(
            {"id": request_id, "method": method, "params": params or {}},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        try:
            self._writer.write(line.encode() + b"\n")
            await self._writer.drain()
            return await asyncio.wait_for(future, timeout)
        except (OSError, ConnectionError) as err:
            raise MelodyConnectionError(f"{method}: {err}") from err
        except TimeoutError as err:
            raise MelodyConnectionError(f"{method}: no answer") from err
        finally:
            self._pending.pop(request_id, None)

    async def _read(self) -> None:
        assert self._reader is not None
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    break
                try:
                    message = json.loads(line)
                except ValueError:
                    _LOGGER.debug("Unreadable line from melodyd: %r", line[:200])
                    continue
                self._dispatch(message)
        except (OSError, ConnectionError, asyncio.IncompleteReadError) as err:
            _LOGGER.debug("Connection to melodyd lost: %s", err)
        except asyncio.LimitOverrunError:
            _LOGGER.warning("melodyd sent a line past %d bytes", _LINE_LIMIT)
        finally:
            self._fail_pending(MelodyConnectionError("the engine went away"))
            if self._writer is not None:
                self._writer.close()
            self._writer = None
            if not self._closing and self._on_disconnect is not None:
                self._on_disconnect()

    def _dispatch(self, message: dict[str, Any]) -> None:
        if "event" in message:
            if self._on_event is not None:
                try:
                    self._on_event(message["event"], message.get("data") or {})
                except Exception:  # noqa: BLE001 - a handler's bug must not end the reading
                    _LOGGER.exception("Handling melodyd event %s failed", message["event"])
            return
        future = self._pending.get(message.get("id", -1))
        if future is None or future.done():
            return
        if error := message.get("error"):
            future.set_exception(
                MelodyError(error.get("message") or error.get("code", "error"), error.get("code"))
            )
        else:
            future.set_result(message.get("result"))

    def _fail_pending(self, error: MelodyError) -> None:
        for future in self._pending.values():
            if not future.done():
                future.set_exception(error)
        self._pending.clear()
