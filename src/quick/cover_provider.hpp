// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/engine_client.hpp"

#include <QCache>
#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <functional>
#include <mutex>

namespace trackknife::quick {

// Covers, read by the engine where the files are and scaled there, so only
// a thumbnail travels (catalogue.artwork). QML asks for
// "image://cover/<id>" with an id from forPath() or forAlbum(), which name
// the engine by its session's index.
class CoverProvider final : public QQuickAsyncImageProvider {
  public:
    // Finds a session's connection by its index; called on worker threads,
    // so it must only read what does not change once the window is up.
    using Clients = std::function<const EngineClient*(int session)>;
    explicit CoverProvider(Clients clients);
    CoverProvider(const CoverProvider&) = delete;
    CoverProvider(CoverProvider&&) = delete;
    CoverProvider& operator=(const CoverProvider&) = delete;
    CoverProvider& operator=(CoverProvider&&) = delete;
    ~CoverProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    // A track's cover, by its encoded path; an album's, by its encoded key.
    // Empty for an empty path, so a delegate shows its placeholder.
    [[nodiscard]] static QString forPath(int session, const QString& encoded_path);
    [[nodiscard]] static QString forAlbum(int session, const QString& encoded_key);

  private:
    friend class CoverResponse;
    [[nodiscard]] QImage load(const QString& id, int size);

    Clients clients_;
    QThreadPool pool_;
    std::mutex mutex_;
    // Keyed by id and size. Cost is in kilobytes.
    QCache<QString, QImage> cache_{64 * 1024};
};

} // namespace trackknife::quick
