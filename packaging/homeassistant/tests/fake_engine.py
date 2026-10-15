# SPDX-License-Identifier: GPL-3.0-only
"""A melodyd that answers the integration over a real socket, as protocol v1 does."""

from __future__ import annotations

import asyncio
import base64
import json
from typing import Any

PASSWORD = "correct horse"
ENGINE_ID = "8a2a8d1e-1d8e-4b54-9c41-5a8e1f0a3b77"
COVER = b"\x89PNG\r\n\x1a\nfake cover"


def encode(path: str) -> str:
    """A raw path as the protocol carries it."""
    return base64.b64encode(path.encode()).decode()


class FakeEngine:
    """Enough of melodyd: authentication, playback, outputs, events."""

    def __init__(self) -> None:
        """Stopped, with two outputs."""
        self.sequence = 0
        self.state: dict[str, Any] = {
            "status": "stopped",
            "entry": None,
            "path": None,
            "position_ms": 0,
            "duration_ms": -1,
            "volume_percent": 50,
            "modes": {"repeat": False, "random": False, "single": 0, "consume": 0},
        }
        self.track: dict[str, Any] | None = None
        self.outputs = [
            {"id": "local", "name": "This computer", "online": True, "selected": True},
            {"id": "agent:bedside", "name": "bedside", "online": True, "selected": False},
        ]
        self.calls: list[tuple[str, dict[str, Any]]] = []
        self._writers: set[asyncio.StreamWriter] = set()
        self._server: asyncio.Server | None = None
        self.port = 0

    async def start(self) -> None:
        """Listen on a free local port."""
        self._server = await asyncio.start_server(self._serve, "127.0.0.1", 0)
        self.port = self._server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        """Hang up on everyone and stop listening."""
        for writer in list(self._writers):
            writer.close()
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    def snapshot(self) -> dict[str, Any]:
        """The state as playback.state and playback.changed carry it."""
        self.sequence += 1
        return {**self.state, "sequence": self.sequence}

    def play(self, path: str, entry: str, track: dict[str, Any]) -> None:
        """Start a track, and say so."""
        self.state.update(
            status="playing", entry=entry, path=encode(path), position_ms=0,
            duration_ms=track.get("duration_ms", -1),
        )
        self.track = {**track, "entry": entry, "path": encode(path)}
        self.emit("playback.changed", self.snapshot())

    def emit(self, name: str, data: dict[str, Any]) -> None:
        """An event to every client admitted."""
        line = json.dumps({"event": name, "data": data}).encode() + b"\n"
        for writer in list(self._writers):
            writer.write(line)

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        admitted = False
        try:
            while line := await reader.readline():
                request = json.loads(line)
                method, params = request["method"], request.get("params") or {}
                if not admitted:
                    if method == "session.authenticate" and params.get("password") == PASSWORD:
                        admitted = True
                        self._writers.add(writer)
                        self._answer(writer, request, {"authenticated": True})
                        continue
                    self._error(writer, request, "unauthorized", "wrong password")
                    break
                self.calls.append((method, params))
                self._answer(writer, request, self._handle(method, params))
                await writer.drain()
        finally:
            self._writers.discard(writer)
            writer.close()

    def _handle(self, method: str, params: dict[str, Any]) -> Any:
        state_changes = {
            "playback.pause": {"status": "paused"},
            "playback.resume": {"status": "playing"},
            "playback.stop": {"status": "stopped"},
        }
        if method == "engine.info":
            return {"name": "gemenon", "id": ENGINE_ID, "protocol": 1, "level": 13,
                    "release": "0.3.0"}
        if method == "playback.state":
            return self.snapshot()
        if method in state_changes:
            self.state.update(state_changes[method])
            snapshot = self.snapshot()
            self.emit("playback.changed", snapshot)
            return snapshot
        if method in ("playback.next", "playback.previous"):
            return self.snapshot()
        if method == "playback.seek":
            self.state["position_ms"] = params["position_ms"]
            return self.snapshot()
        if method == "playback.set_volume":
            self.state["volume_percent"] = params["percent"]
            return self.snapshot()
        if method == "playback.set_modes":
            self.state["modes"] = {**self.state["modes"], **params}
            return self.snapshot()
        if method == "playback.now_playing":
            return {"track": self.track}
        if method == "catalogue.artwork":
            return {"image": base64.b64encode(COVER).decode()}
        if method == "outputs.list":
            return {"outputs": self.outputs}
        if method == "outputs.select":
            for output in self.outputs:
                output["selected"] = output["id"] == params["id"]
            return {}
        return {}

    @staticmethod
    def _answer(writer: asyncio.StreamWriter, request: dict[str, Any], result: Any) -> None:
        writer.write(json.dumps({"id": request["id"], "result": result}).encode() + b"\n")

    @staticmethod
    def _error(writer: asyncio.StreamWriter, request: dict[str, Any], code: str, message: str) -> None:
        writer.write(
            json.dumps({"id": request["id"], "error": {"code": code, "message": message, "context": {}}}).encode()
            + b"\n"
        )
