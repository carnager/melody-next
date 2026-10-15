# SPDX-License-Identifier: GPL-3.0-only
"""Against a real melodyd, when MELODYD names one: MELODYD=build/dev/src/daemon/melodyd.

The engine runs --local-only -- no TCP, so nothing announced on the network
-- and a bridge here carries the integration's TCP to its socket, where the
engine takes a client as admitted already and says so to its password.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncGenerator
import contextlib
import os
from pathlib import Path
import tempfile

import pytest
import pytest_socket

from homeassistant.components.media_player import (
    ATTR_INPUT_SOURCE_LIST,
    ATTR_MEDIA_VOLUME_LEVEL,
    DOMAIN as MEDIA_PLAYER,
)
from homeassistant.const import (
    ATTR_ENTITY_ID,
    CONF_HOST,
    CONF_PASSWORD,
    CONF_PORT,
    SERVICE_VOLUME_SET,
    STATE_IDLE,
)
from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.melody.const import DOMAIN

pytestmark = pytest.mark.skipif(not os.environ.get("MELODYD"), reason="MELODYD is not set")


async def _pipe(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    with contextlib.suppress(OSError, ConnectionError):
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    writer.close()


@pytest.fixture
async def real_engine() -> AsyncGenerator[int]:
    """A melodyd on its socket, reached through a TCP port here."""
    # The bridge reaches the engine's Unix socket; still nothing off this machine.
    pytest_socket.socket_allow_hosts(["127.0.0.1"], allow_unix_socket=True)
    with tempfile.TemporaryDirectory(prefix="melody-ha-", dir="/tmp") as directory:
        socket = Path(directory) / "m.sock"
        process = await asyncio.create_subprocess_exec(
            os.environ["MELODYD"],
            "--socket", str(socket),
            "--state", str(Path(directory) / "state"),
            "--name", "test-engine",
            "--local-only",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        for _ in range(200):
            if socket.exists():
                break
            await asyncio.sleep(0.02)

        async def bridge(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            engine_reader, engine_writer = await asyncio.open_unix_connection(str(socket))
            await asyncio.gather(_pipe(reader, engine_writer), _pipe(engine_reader, writer))

        server = await asyncio.start_server(bridge, "127.0.0.1", 0)
        try:
            yield server.sockets[0].getsockname()[1]
        finally:
            server.close()
            process.terminate()
            await process.wait()


async def test_the_real_engine(hass: HomeAssistant, real_engine: int) -> None:
    """Its state, its outputs and its volume, as melodyd answers them."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id="real",
        title="test-engine",
        data={CONF_HOST: "127.0.0.1", CONF_PORT: real_engine, CONF_PASSWORD: "unused"},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.runtime_data.info["level"] >= 13
    player = "media_player.test_engine"
    state = hass.states.get(player)
    assert state.state == STATE_IDLE
    assert state.attributes[ATTR_INPUT_SOURCE_LIST]
    assert await entry.runtime_data.call("playback.now_playing") == {"track": None}
    await hass.services.async_call(
        MEDIA_PLAYER,
        SERVICE_VOLUME_SET,
        {ATTR_ENTITY_ID: player, ATTR_MEDIA_VOLUME_LEVEL: 0.42},
        blocking=True,
    )
    assert hass.states.get(player).attributes[ATTR_MEDIA_VOLUME_LEVEL] == 0.42
    assert await hass.config_entries.async_unload(entry.entry_id)
