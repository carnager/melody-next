// SPDX-License-Identifier: GPL-3.0-only

#include "quick/cover_provider.hpp"

#include <QQuickTextureFactory>
#include <QRunnable>

namespace trackknife::quick {

namespace {

// Encoded paths and keys are base64, whose '/' and '+' do not survive a
// URL's path; the id carries the URL-safe alphabet and is turned back here.
[[nodiscard]] QString urlSafe(QString encoded) {
    return encoded.replace(QLatin1Char('+'), QLatin1Char('-')).replace(QLatin1Char('/'), QLatin1Char('_'));
}

[[nodiscard]] std::string standard(QString encoded) {
    return encoded.replace(QLatin1Char('-'), QLatin1Char('+')).replace(QLatin1Char('_'), QLatin1Char('/')).toStdString();
}

} // namespace

class CoverResponse final : public QQuickImageResponse, public QRunnable {
  public:
    CoverResponse(CoverProvider& provider, QString id, const int size)
        : provider_(provider), id_(std::move(id)), size_(size) {
        setAutoDelete(false);
    }

    [[nodiscard]] QQuickTextureFactory* textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }

    void run() override {
        image_ = provider_.load(id_, size_);
        emit finished();
    }

  private:
    CoverProvider& provider_;
    QString id_;
    int size_;
    QImage image_;
};

CoverProvider::CoverProvider(Clients clients) : clients_(std::move(clients)) { pool_.setMaxThreadCount(4); }

CoverProvider::~CoverProvider() {
    pool_.clear();
    pool_.waitForDone();
}

QString CoverProvider::forPath(const int session, const QString& encoded_path) {
    return encoded_path.isEmpty() ? QString{}
                                  : QStringLiteral("image://cover/%1/path/").arg(session) + urlSafe(encoded_path);
}

QString CoverProvider::forAlbum(const int session, const QString& encoded_key) {
    return encoded_key.isEmpty() ? QString{}
                                 : QStringLiteral("image://cover/%1/album/").arg(session) + urlSafe(encoded_key);
}

QQuickImageResponse* CoverProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    // Asked for at the size shown, doubled for high-density screens; one
    // size bucket per 64 pixels keeps the cache from filling with near
    // duplicates as a panel is resized.
    const auto wanted = std::max(requestedSize.width(), requestedSize.height());
    const auto size = wanted <= 0 ? 256 : std::min(1024, ((wanted * 2 + 63) / 64) * 64);
    auto* response = new CoverResponse(*this, id, size);
    pool_.start(response);
    return response;
}

QImage CoverProvider::load(const QString& id, const int size) {
    const auto key = id + QLatin1Char('@') + QString::number(size);
    {
        const std::lock_guard guard{mutex_};
        if (const auto* cached = cache_.object(key)) {
            return *cached;
        }
    }
    // <session>/<album|path>/<encoded>
    const auto first = id.indexOf(QLatin1Char('/'));
    const auto second = id.indexOf(QLatin1Char('/'), first + 1);
    if (first < 0 || second < 0) {
        return {};
    }
    const auto* client = clients_(id.left(first).toInt());
    if (client == nullptr) {
        return {};
    }
    const auto kind = id.mid(first + 1, second - first - 1);
    const auto encoded = standard(id.mid(second + 1));
    protocol::Json params = protocol::Json::object();
    if (kind == QStringLiteral("album")) {
        params["album_key"] = encoded;
    } else if (kind == QStringLiteral("path")) {
        params["path"] = encoded;
    } else {
        return {};
    }
    params["size"] = size;
    QImage image;
    if (const auto answer = client->callNow("catalogue.artwork", params)) {
        if (const auto found = answer->find("image"); found != answer->end() && found->is_string()) {
            if (const auto bytes = protocol::decode_raw_path(found->get<std::string>())) {
                image = QImage::fromData(reinterpret_cast<const uchar*>(bytes->data()),
                                         static_cast<int>(bytes->size()));
            }
        }
    } else {
        // Not connected yet, or the engine is busy: not cached, so the next
        // delegate to ask tries again.
        return {};
    }
    const std::lock_guard guard{mutex_};
    // A cover that is not there is remembered too, as a null image, so a
    // scrolling list does not ask again for every album without art.
    cache_.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

} // namespace trackknife::quick
