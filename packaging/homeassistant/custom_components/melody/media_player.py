# SPDX-License-Identifier: GPL-3.0-only
"""The engine's player: what plays, how loud, and on which speakers."""

from __future__ import annotations

import base64
import hashlib
import logging
from typing import Any

from homeassistant.components.media_player import (
    MediaPlayerDeviceClass,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
    MediaType,
    RepeatMode,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.util import dt as dt_util

from . import MelodyConfigEntry, MelodyConnection
from .client import MelodyError
from .const import DOMAIN, MINIMUM_LEVEL

_LOGGER = logging.getLogger(__name__)

_STATES = {
    "playing": MediaPlayerState.PLAYING,
    "paused": MediaPlayerState.PAUSED,
    "loading": MediaPlayerState.BUFFERING,
    "stopped": MediaPlayerState.IDLE,
}

# Covers are asked for at this size: the engine scales them where the file is.
_COVER_SIZE = 600


async def async_setup_entry(
    hass: HomeAssistant,
    entry: MelodyConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """The engine's one player."""
    async_add_entities([MelodyPlayer(entry.runtime_data, entry.unique_id or entry.entry_id)])


def _image_type(data: bytes) -> str:
    if data.startswith(b"\x89PNG"):
        return "image/png"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    if data.startswith(b"GIF8"):
        return "image/gif"
    return "image/jpeg"


class MelodyPlayer(MediaPlayerEntity):
    """melodyd's playback, as its events tell it."""

    _attr_has_entity_name = True
    _attr_name = None
    _attr_should_poll = False
    _attr_device_class = MediaPlayerDeviceClass.SPEAKER
    _attr_media_content_type = MediaType.MUSIC
    _attr_supported_features = (
        MediaPlayerEntityFeature.PLAY
        | MediaPlayerEntityFeature.PAUSE
        | MediaPlayerEntityFeature.STOP
        | MediaPlayerEntityFeature.NEXT_TRACK
        | MediaPlayerEntityFeature.PREVIOUS_TRACK
        | MediaPlayerEntityFeature.SEEK
        | MediaPlayerEntityFeature.VOLUME_SET
        | MediaPlayerEntityFeature.VOLUME_STEP
        | MediaPlayerEntityFeature.SHUFFLE_SET
        | MediaPlayerEntityFeature.REPEAT_SET
        | MediaPlayerEntityFeature.SELECT_SOURCE
    )

    def __init__(self, connection: MelodyConnection, engine_id: str) -> None:
        """A player for the engine known by engine_id."""
        self._connection = connection
        self._attr_unique_id = engine_id
        info = connection.info
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, engine_id)},
            name=info.get("name") or "melodyd",
            manufacturer="Melody",
            model="melodyd",
            sw_version=info.get("release") or None,
        )
        self._sequence = -1
        # The entry whose fields are shown; asked again when another plays.
        self._shown: tuple[str | None, str | None] = (None, None)
        self._outputs: list[dict[str, Any]] = []
        self._cover_path: str | None = None

    @property
    def available(self) -> bool:
        """Whether the engine is connected."""
        return self._connection.available

    async def async_added_to_hass(self) -> None:
        """Hear the engine, and learn where it stands."""
        self.async_on_remove(self._connection.listen(self._event))
        self.async_on_remove(self._connection.watch_availability(self._availability))
        if (level := self._connection.info.get("level", 0)) < MINIMUM_LEVEL:
            _LOGGER.warning(
                "melodyd %s speaks protocol level %s; track details need %s or newer",
                self._connection.host,
                level,
                MINIMUM_LEVEL,
            )
        await self._refresh()

    async def _refresh(self) -> None:
        try:
            state = await self._connection.call("playback.state")
            outputs = await self._connection.call("outputs.list")
        except MelodyError as err:
            _LOGGER.debug("Reading melodyd's state failed: %s", err)
            return
        self._outputs = outputs.get("outputs", [])
        self._sequence = -1
        self._shown = (None, None)
        self._apply_state(state)
        self._apply_outputs()
        self.async_write_ha_state()

    @callback
    def _availability(self, available: bool) -> None:
        if available:
            self.hass.async_create_task(self._refresh())
        else:
            self.async_write_ha_state()

    @callback
    def _event(self, name: str, data: dict[str, Any]) -> None:
        if name == "playback.changed":
            self._apply_state(data)
            self.async_write_ha_state()
        elif name == "outputs.changed":
            self.hass.async_create_task(self._refresh_outputs())

    async def _refresh_outputs(self) -> None:
        try:
            outputs = await self._connection.call("outputs.list")
        except MelodyError:
            return
        self._outputs = outputs.get("outputs", [])
        self._apply_outputs()
        self.async_write_ha_state()

    @callback
    def _apply_outputs(self) -> None:
        shown = [output for output in self._outputs if output.get("online", True)]
        self._attr_source_list = [output["name"] for output in shown]
        selected = next((output for output in self._outputs if output.get("selected")), None)
        self._attr_source = selected["name"] if selected else None

    @callback
    def _apply_state(self, state: dict[str, Any]) -> None:
        # States can cross -- an event and a command's answer -- so the later
        # one wins, not whichever came last.
        sequence = state.get("sequence", 0)
        if sequence < self._sequence:
            return
        self._sequence = sequence
        self._attr_state = _STATES.get(state.get("status", ""), MediaPlayerState.IDLE)
        self._attr_volume_level = state.get("volume_percent", 0) / 100
        duration = state.get("duration_ms", -1)
        self._attr_media_duration = duration / 1000 if duration and duration > 0 else None
        self._attr_media_position = state.get("position_ms", 0) / 1000
        self._attr_media_position_updated_at = dt_util.utcnow()
        modes = state.get("modes") or {}
        self._attr_shuffle = bool(modes.get("random") or modes.get("album_random"))
        if modes.get("repeat") and modes.get("single"):
            self._attr_repeat = RepeatMode.ONE
        elif modes.get("repeat"):
            self._attr_repeat = RepeatMode.ALL
        else:
            self._attr_repeat = RepeatMode.OFF
        playing = (state.get("entry"), state.get("path"))
        if playing != self._shown:
            self._shown = playing
            if playing[1] is None:
                self._clear_track()
            else:
                self.hass.async_create_task(self._read_track(playing))

    @callback
    def _clear_track(self) -> None:
        self._attr_media_title = None
        self._attr_media_artist = None
        self._attr_media_album_name = None
        self._attr_media_album_artist = None
        self._attr_media_track = None
        self._attr_media_content_id = None
        self._attr_media_image_hash = None
        self._cover_path = None

    async def _read_track(self, playing: tuple[str | None, str | None]) -> None:
        try:
            answer = await self._connection.call("playback.now_playing")
        except MelodyError as err:
            _LOGGER.debug("Asking melodyd what plays failed: %s", err)
            return
        if self._shown != playing:
            return  # Another started meanwhile; its own question is on its way.
        track = (answer or {}).get("track")
        if not track:
            self._clear_track()
        else:
            self._attr_media_title = track.get("title")
            self._attr_media_artist = track.get("artist")
            self._attr_media_album_name = track.get("album")
            self._attr_media_album_artist = track.get("album_artist")
            number = track.get("track_number")
            try:
                self._attr_media_track = int(str(number).split("/", 1)[0]) if number else None
            except ValueError:
                self._attr_media_track = None
            self._attr_media_content_id = track.get("path")
            self._cover_path = track.get("path")
            # Changes with the file, so a new track's cover is asked for.
            self._attr_media_image_hash = (
                hashlib.sha256(self._cover_path.encode()).hexdigest()[:16]
                if self._cover_path
                else None
            )
        self.async_write_ha_state()

    async def async_get_media_image(self) -> tuple[bytes | None, str | None]:
        """The playing track's cover, from the engine."""
        if self._cover_path is None:
            return None, None
        try:
            answer = await self._connection.call(
                "catalogue.artwork", {"path": self._cover_path, "size": _COVER_SIZE}
            )
        except MelodyError as err:
            _LOGGER.debug("No cover from melodyd: %s", err)
            return None, None
        encoded = (answer or {}).get("image")
        if not encoded:
            return None, None
        data = base64.b64decode(encoded)
        return data, _image_type(data)

    async def _command(self, method: str, params: dict[str, Any] | None = None) -> None:
        try:
            answer = await self._connection.call(method, params)
        except MelodyError as err:
            raise HomeAssistantError(f"melodyd: {err}") from err
        # Most commands answer with the state they left.
        if isinstance(answer, dict) and "status" in answer and "sequence" in answer:
            self._apply_state(answer)
            self.async_write_ha_state()

    async def async_media_play(self) -> None:
        """Play, or go on playing."""
        await self._command("playback.resume")

    async def async_media_pause(self) -> None:
        """Pause."""
        await self._command("playback.pause")

    async def async_media_stop(self) -> None:
        """Stop."""
        await self._command("playback.stop")

    async def async_media_next_track(self) -> None:
        """The next track."""
        await self._command("playback.next")

    async def async_media_previous_track(self) -> None:
        """The previous track."""
        await self._command("playback.previous")

    async def async_media_seek(self, position: float) -> None:
        """Go to a place in the track."""
        await self._command("playback.seek", {"position_ms": round(position * 1000)})

    async def async_set_volume_level(self, volume: float) -> None:
        """Set how loud, 0 to 1."""
        await self._command("playback.set_volume", {"percent": round(volume * 100)})

    async def async_set_shuffle(self, shuffle: bool) -> None:
        """Shuffle, or not."""
        await self._command("playback.set_modes", {"random": shuffle})

    async def async_set_repeat(self, repeat: RepeatMode) -> None:
        """Repeat the list, the track, or nothing."""
        await self._command(
            "playback.set_modes",
            {"repeat": repeat != RepeatMode.OFF, "single": 1 if repeat == RepeatMode.ONE else 0},
        )

    async def async_select_source(self, source: str) -> None:
        """Play on another output: an agent, a renderer, a sound card."""
        output = next((o for o in self._outputs if o.get("name") == source), None)
        if output is None:
            raise HomeAssistantError(f"melodyd has no output called {source}")
        await self._command("outputs.select", {"id": output["id"]})
        await self._refresh_outputs()
