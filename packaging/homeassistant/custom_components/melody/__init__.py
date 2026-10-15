# SPDX-License-Identifier: GPL-3.0-only
"""Melody: a melodyd engine as a media player."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import contextlib
from dataclasses import dataclass, field
import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT, Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed, ConfigEntryNotReady

from .client import MelodyAuthError, MelodyClient, MelodyConnectionError, MelodyError
from .const import DEFAULT_PORT

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.MEDIA_PLAYER]

# Waited between attempts to reach an engine that went away.
_RETRY_DELAYS = (2, 5, 10, 30)

type MelodyConfigEntry = ConfigEntry[MelodyConnection]


@dataclass
class MelodyConnection:
    """One engine, kept connected: reconnects when it goes away."""

    hass: HomeAssistant
    host: str
    port: int
    password: str | None
    info: dict[str, Any] = field(default_factory=dict)
    client: MelodyClient | None = None
    _listeners: list[Callable[[str, dict[str, Any]], None]] = field(default_factory=list)
    _availability: list[Callable[[bool], None]] = field(default_factory=list)
    _reconnecting: asyncio.Task[None] | None = None
    _stopped: bool = False

    @property
    def available(self) -> bool:
        """Whether the engine is connected now."""
        return self.client is not None and self.client.connected

    async def connect(self) -> None:
        """Reach the engine and learn who it is."""
        client = MelodyClient(
            self.host,
            self.port,
            self.password,
            on_event=self._event,
            on_disconnect=self._lost,
        )
        await client.connect()
        self.client = client
        self.info = await client.call("engine.info")

    async def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        """Ask the engine; raises MelodyError when it cannot be asked."""
        if self.client is None:
            raise MelodyConnectionError("not connected")
        return await self.client.call(method, params)

    @callback
    def listen(self, handler: Callable[[str, dict[str, Any]], None]) -> Callable[[], None]:
        """Hear the engine's events; returns what stops hearing them."""
        self._listeners.append(handler)
        return lambda: self._listeners.remove(handler)

    @callback
    def watch_availability(self, handler: Callable[[bool], None]) -> Callable[[], None]:
        """Be told when the engine comes and goes."""
        self._availability.append(handler)
        return lambda: self._availability.remove(handler)

    async def stop(self) -> None:
        """Hang up for good."""
        self._stopped = True
        if self._reconnecting is not None:
            self._reconnecting.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._reconnecting
        if self.client is not None:
            await self.client.close()

    @callback
    def _event(self, name: str, data: dict[str, Any]) -> None:
        for handler in list(self._listeners):
            handler(name, data)

    @callback
    def _lost(self) -> None:
        if self._stopped:
            return
        _LOGGER.info("Lost melodyd at %s:%s; reconnecting", self.host, self.port)
        for handler in list(self._availability):
            handler(False)
        if self._reconnecting is None or self._reconnecting.done():
            self._reconnecting = self.hass.async_create_background_task(
                self._reconnect(), "melody reconnect"
            )

    async def _reconnect(self) -> None:
        attempt = 0
        while not self._stopped:
            await asyncio.sleep(_RETRY_DELAYS[min(attempt, len(_RETRY_DELAYS) - 1)])
            attempt += 1
            try:
                await self.connect()
            except MelodyAuthError:
                _LOGGER.warning("melodyd at %s refused the password", self.host)
                continue
            except MelodyError as err:
                _LOGGER.debug("melodyd at %s still away: %s", self.host, err)
                continue
            _LOGGER.info("Reconnected to melodyd at %s:%s", self.host, self.port)
            for handler in list(self._availability):
                handler(True)
            return


async def async_setup_entry(hass: HomeAssistant, entry: MelodyConfigEntry) -> bool:
    """Connect to the engine and add its player."""
    connection = MelodyConnection(
        hass,
        entry.data[CONF_HOST],
        entry.data.get(CONF_PORT, DEFAULT_PORT),
        entry.data.get(CONF_PASSWORD),
    )
    try:
        await connection.connect()
    except MelodyAuthError as err:
        raise ConfigEntryAuthFailed(str(err)) from err
    except MelodyError as err:
        await connection.stop()
        raise ConfigEntryNotReady(str(err)) from err
    entry.runtime_data = connection
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: MelodyConfigEntry) -> bool:
    """Remove the player and hang up."""
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded:
        await entry.runtime_data.stop()
    return unloaded
