// SPDX-License-Identifier: GPL-3.0-only

#include "bench/desktop_media_session.hpp"

#ifdef __APPLE__
#include "bench/now_playing_macos.hpp"
#else
#include "bench/mpris_service.hpp"
#endif

namespace trackknife::bench {

DesktopMediaSession* createDesktopMediaSession(QObject* parent) {
#ifdef __APPLE__
    return new NowPlayingSession(parent);
#else
    return new MprisService(parent);
#endif
}

} // namespace trackknife::bench
