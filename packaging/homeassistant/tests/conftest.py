# SPDX-License-Identifier: GPL-3.0-only
"""Fixtures: Home Assistant with this integration, and an engine to talk to."""

from __future__ import annotations

from collections.abc import AsyncGenerator

import pytest

from .fake_engine import FakeEngine


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations: None) -> None:
    """Load the integration from custom_components."""


@pytest.fixture(autouse=True)
def auto_enable_sockets(socket_enabled: None) -> None:
    """Talk to the engine over a real socket, on this machine only."""


@pytest.fixture
async def engine() -> AsyncGenerator[FakeEngine]:
    """A melodyd listening on a free port."""
    fake = FakeEngine()
    await fake.start()
    yield fake
    await fake.stop()
