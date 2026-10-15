# SPDX-License-Identifier: GPL-3.0-only
"""Adding an engine."""

from __future__ import annotations

from ipaddress import ip_address

from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_PASSWORD, CONF_PORT
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from custom_components.melody.const import DOMAIN

from .fake_engine import ENGINE_ID, PASSWORD, FakeEngine


async def test_by_hand(hass: HomeAssistant, engine: FakeEngine) -> None:
    """Named by hand: known by the id it gives."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_USER}
    )
    assert result["type"] is FlowResultType.FORM
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {CONF_HOST: "127.0.0.1", CONF_PORT: engine.port, CONF_PASSWORD: PASSWORD},
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == "gemenon"
    assert result["result"].unique_id == ENGINE_ID


async def test_wrong_password_and_nobody_there(hass: HomeAssistant, engine: FakeEngine) -> None:
    """A wrong password and an empty port say which went wrong."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_USER}
    )
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {CONF_HOST: "127.0.0.1", CONF_PORT: engine.port, CONF_PASSWORD: "wrong"},
    )
    assert result["errors"] == {"base": "invalid_auth"}
    port = engine.port
    await engine.stop()
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: port, CONF_PASSWORD: PASSWORD}
    )
    assert result["errors"] == {"base": "cannot_connect"}


def _announced(port: int) -> ZeroconfServiceInfo:
    return ZeroconfServiceInfo(
        ip_address=ip_address("127.0.0.1"),
        ip_addresses=[ip_address("127.0.0.1")],
        port=port,
        hostname="gemenon.local.",
        type="_melody._tcp.local.",
        name="gemenon._melody._tcp.local.",
        properties={"id": ENGINE_ID, "proto": "1", "auth": "1", "http": "6680"},
    )


async def test_found_on_the_network(hass: HomeAssistant, engine: FakeEngine) -> None:
    """Announced: only its password is asked, and it is found once."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_announced(engine.port)
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "zeroconf_confirm"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_PASSWORD: PASSWORD}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {CONF_HOST: "127.0.0.1", CONF_PORT: engine.port, CONF_PASSWORD: PASSWORD}
    await hass.async_block_till_done()

    again = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=_announced(engine.port)
    )
    assert again["type"] is FlowResultType.ABORT
    assert again["reason"] == "already_configured"
