# SPDX-License-Identifier: GPL-3.0-only
"""Constants of the Melody integration."""

from typing import Final

DOMAIN: Final = "melody"
DEFAULT_PORT: Final = 6603
# What melodyd announces itself as over mDNS.
ZEROCONF_TYPE: Final = "_melody._tcp.local."
# The protocol level that answers playback.now_playing.
MINIMUM_LEVEL: Final = 13
