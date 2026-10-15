# SPDX-License-Identifier: GPL-3.0-only
"""Adding a melodyd engine: found on the network, or named by hand."""

from __future__ import annotations

from collections.abc import Mapping
import logging
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .client import MelodyAuthError, MelodyClient, MelodyError
from .const import DEFAULT_PORT, DOMAIN

_LOGGER = logging.getLogger(__name__)


async def _engine_info(host: str, port: int, password: str) -> dict[str, Any]:
    """Who the engine at host:port is; raises MelodyError."""
    client = MelodyClient(host, port, password)
    await client.connect()
    try:
        return await client.call("engine.info")
    finally:
        await client.close()


def _errors_of(err: Exception) -> dict[str, str]:
    if isinstance(err, MelodyAuthError):
        return {"base": "invalid_auth"}
    if isinstance(err, MelodyError):
        return {"base": "cannot_connect"}
    _LOGGER.exception("Unexpected error asking melodyd")
    return {"base": "unknown"}


class MelodyConfigFlow(ConfigFlow, domain=DOMAIN):
    """One engine, one entry, known by the id it announces."""

    VERSION = 1

    def __init__(self) -> None:
        """Nothing found yet."""
        self._host: str | None = None
        self._port: int = DEFAULT_PORT
        self._name: str | None = None

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """An engine named by hand."""
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                info = await _engine_info(
                    user_input[CONF_HOST], user_input[CONF_PORT], user_input[CONF_PASSWORD]
                )
            except Exception as err:  # noqa: BLE001 - shown in the form
                errors = _errors_of(err)
            else:
                await self.async_set_unique_id(info["id"])
                self._abort_if_unique_id_configured(
                    updates={CONF_HOST: user_input[CONF_HOST], CONF_PORT: user_input[CONF_PORT]}
                )
                return self.async_create_entry(title=info.get("name") or "melodyd", data=user_input)
        return self.async_show_form(
            step_id="user",
            data_schema=self.add_suggested_values_to_schema(
                vol.Schema(
                    {
                        vol.Required(CONF_HOST): str,
                        vol.Required(CONF_PORT, default=DEFAULT_PORT): int,
                        vol.Required(CONF_PASSWORD): str,
                    }
                ),
                user_input,
            ),
            errors=errors,
        )

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """An engine announcing itself: only its password is asked."""
        engine_id = discovery_info.properties.get("id")
        if not engine_id or discovery_info.properties.get("proto") != "1":
            return self.async_abort(reason="not_supported")
        self._host = discovery_info.host
        self._port = discovery_info.port or DEFAULT_PORT
        self._name = discovery_info.name.split(".", 1)[0]
        await self.async_set_unique_id(engine_id)
        # Known already: where it is now, if it moved.
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host, CONF_PORT: self._port})
        self.context["title_placeholders"] = {"name": self._name}
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """The found engine's password."""
        assert self._host is not None
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                info = await _engine_info(self._host, self._port, user_input[CONF_PASSWORD])
            except Exception as err:  # noqa: BLE001 - shown in the form
                errors = _errors_of(err)
            else:
                return self.async_create_entry(
                    title=info.get("name") or self._name or "melodyd",
                    data={
                        CONF_HOST: self._host,
                        CONF_PORT: self._port,
                        CONF_PASSWORD: user_input[CONF_PASSWORD],
                    },
                )
        return self.async_show_form(
            step_id="zeroconf_confirm",
            data_schema=vol.Schema({vol.Required(CONF_PASSWORD): str}),
            description_placeholders={"name": self._name or "", "host": self._host},
            errors=errors,
        )

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        """The engine no longer takes the password it was given."""
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Its new password."""
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                await _engine_info(
                    entry.data[CONF_HOST],
                    entry.data.get(CONF_PORT, DEFAULT_PORT),
                    user_input[CONF_PASSWORD],
                )
            except Exception as err:  # noqa: BLE001 - shown in the form
                errors = _errors_of(err)
            else:
                return self.async_update_reload_and_abort(
                    entry, data_updates={CONF_PASSWORD: user_input[CONF_PASSWORD]}
                )
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_PASSWORD): str}),
            description_placeholders={"name": entry.title},
            errors=errors,
        )
