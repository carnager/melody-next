# SPDX-License-Identifier: GPL-3.0-only
"""The engine's player, against an engine over a socket."""

from __future__ import annotations

from unittest.mock import patch

from homeassistant.components.media_player import (
    ATTR_INPUT_SOURCE,
    ATTR_INPUT_SOURCE_LIST,
    ATTR_MEDIA_ALBUM_NAME,
    ATTR_MEDIA_ARTIST,
    ATTR_MEDIA_DURATION,
    ATTR_MEDIA_REPEAT,
    ATTR_MEDIA_SEEK_POSITION,
    ATTR_MEDIA_SHUFFLE,
    ATTR_MEDIA_TITLE,
    ATTR_MEDIA_VOLUME_LEVEL,
    DOMAIN as MEDIA_PLAYER,
    SERVICE_SELECT_SOURCE,
)
from homeassistant.const import (
    ATTR_ENTITY_ID,
    ATTR_ENTITY_PICTURE,
    CONF_HOST,
    CONF_PASSWORD,
    CONF_PORT,
    SERVICE_MEDIA_PAUSE,
    SERVICE_MEDIA_SEEK,
    SERVICE_REPEAT_SET,
    SERVICE_SHUFFLE_SET,
    SERVICE_VOLUME_SET,
    STATE_IDLE,
    STATE_PAUSED,
    STATE_PLAYING,
    STATE_UNAVAILABLE,
)
from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import MockConfigEntry
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator

from custom_components.melody.const import DOMAIN

from .fake_engine import COVER, ENGINE_ID, PASSWORD, FakeEngine

PLAYER = "media_player.gemenon"
TITLE = "בגילגול"


async def _added(hass: HomeAssistant, engine: FakeEngine) -> MockConfigEntry:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=ENGINE_ID,
        title="gemenon",
        data={CONF_HOST: "127.0.0.1", CONF_PORT: engine.port, CONF_PASSWORD: PASSWORD},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def _until(hass: HomeAssistant, check, tries: int = 100) -> None:
    import asyncio

    for _ in range(tries):
        await hass.async_block_till_done()
        if check():
            return
        await asyncio.sleep(0.02)
    raise AssertionError("not reached")


async def test_what_plays_shows(
    hass: HomeAssistant, engine: FakeEngine, hass_client: ClientSessionGenerator
) -> None:
    """Stopped, then a track: its fields, its length, its cover."""
    await _added(hass, engine)
    state = hass.states.get(PLAYER)
    assert state.state == STATE_IDLE
    assert state.attributes[ATTR_MEDIA_VOLUME_LEVEL] == 0.5
    assert state.attributes[ATTR_INPUT_SOURCE_LIST] == ["This computer", "bedside"]
    assert state.attributes[ATTR_INPUT_SOURCE] == "This computer"

    engine.play(
        "/music/Shalom Hanoch/06.flac",
        "entry-1",
        {"title": TITLE, "artist": "Shalom Hanoch", "album": "HaMeitav", "duration_ms": 277_000},
    )
    await _until(hass, lambda: hass.states.get(PLAYER).attributes.get(ATTR_MEDIA_TITLE) == TITLE)
    state = hass.states.get(PLAYER)
    assert state.state == STATE_PLAYING
    assert state.attributes[ATTR_MEDIA_ARTIST] == "Shalom Hanoch"
    assert state.attributes[ATTR_MEDIA_ALBUM_NAME] == "HaMeitav"
    assert state.attributes[ATTR_MEDIA_DURATION] == 277

    client = await hass_client()
    response = await client.get(state.attributes[ATTR_ENTITY_PICTURE])
    assert response.status == 200
    assert response.content_type == "image/png"
    assert await response.read() == COVER


async def test_commands_reach_the_engine(hass: HomeAssistant, engine: FakeEngine) -> None:
    """Pause, seek, volume, modes and outputs, as the engine's methods."""
    await _added(hass, engine)
    engine.play("/music/a.flac", "entry-1", {"title": "A", "duration_ms": 200_000})
    await _until(hass, lambda: hass.states.get(PLAYER).state == STATE_PLAYING)

    async def service(name: str, **data) -> None:
        await hass.services.async_call(
            MEDIA_PLAYER, name, {ATTR_ENTITY_ID: PLAYER, **data}, blocking=True
        )

    await service(SERVICE_MEDIA_PAUSE)
    assert hass.states.get(PLAYER).state == STATE_PAUSED
    await service(SERVICE_MEDIA_SEEK, **{ATTR_MEDIA_SEEK_POSITION: 42.5})
    await service(SERVICE_VOLUME_SET, **{ATTR_MEDIA_VOLUME_LEVEL: 0.3})
    await service(SERVICE_SHUFFLE_SET, **{ATTR_MEDIA_SHUFFLE: True})
    await service(SERVICE_REPEAT_SET, **{ATTR_MEDIA_REPEAT: "one"})
    await service(SERVICE_SELECT_SOURCE, **{ATTR_INPUT_SOURCE: "bedside"})
    assert ("playback.seek", {"position_ms": 42_500}) in engine.calls
    assert ("playback.set_volume", {"percent": 30}) in engine.calls
    assert ("playback.set_modes", {"random": True}) in engine.calls
    assert ("playback.set_modes", {"repeat": True, "single": 1}) in engine.calls
    assert ("outputs.select", {"id": "agent:bedside"}) in engine.calls
    state = hass.states.get(PLAYER)
    assert state.attributes[ATTR_MEDIA_VOLUME_LEVEL] == 0.3
    assert state.attributes[ATTR_MEDIA_SHUFFLE] is True
    assert state.attributes[ATTR_MEDIA_REPEAT] == "one"
    assert state.attributes[ATTR_INPUT_SOURCE] == "bedside"


async def test_an_engine_that_goes_away_comes_back(hass: HomeAssistant, engine: FakeEngine) -> None:
    """Unavailable while it is away; as it is once it is back."""
    with patch("custom_components.melody._RETRY_DELAYS", (0.05,)):
        await _added(hass, engine)
        assert hass.states.get(PLAYER).state == STATE_IDLE
        port = engine.port
        await engine.stop()
        await _until(hass, lambda: hass.states.get(PLAYER).state == STATE_UNAVAILABLE)

        back = FakeEngine()
        back._server = None
        import asyncio

        back._server = await asyncio.start_server(back._serve, "127.0.0.1", port)
        back.port = port
        back.play("/music/b.flac", "entry-2", {"title": "B"})
        try:
            await _until(hass, lambda: hass.states.get(PLAYER).state == STATE_PLAYING, tries=200)
            await _until(
                hass, lambda: hass.states.get(PLAYER).attributes.get(ATTR_MEDIA_TITLE) == "B"
            )
        finally:
            await back.stop()
