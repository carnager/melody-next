// SPDX-License-Identifier: GPL-3.0-only

#include "bench/file_work_tools.hpp"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {

FileWorkTools engineFileWorkTools(std::shared_ptr<engine::RemoteFileWork> work) {
    return FileWorkTools{
        .access = work->access(),
        .scanner =
            [work](std::span<const loudness::LoudnessScanItem> items,
                   const loudness::LoudnessScanOptions& options,
                   const loudness::LoudnessScanProgressCallback& progress,
                   const core::CancellationToken& cancellation) {
                return work->scan(items, options, progress, cancellation);
            },
        .probe =
            [work](const std::string& raw_path, const core::CancellationToken& cancellation) {
                return work->probe(raw_path, cancellation);
            }};
}

namespace {

// Runs `work` off this thread and hands its answer to `completion` on
// `context`'s; dropped when `context` is gone.
template <typename T, typename Work>
void answered_on(QObject* context, Work work, std::function<void(core::Result<T>)> completion) {
    const QPointer<QObject> guard{context};
    static_cast<void>(QtConcurrent::run(
        [guard, work = std::move(work), completion = std::move(completion)]() mutable {
            auto answer = work();
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                guard.data(),
                [answer = std::move(answer), completion = std::move(completion)]() mutable {
                    completion(std::move(answer));
                },
                Qt::QueuedConnection);
        }));
}

[[nodiscard]] QByteArray bytes_of(const std::string& body) {
    return QByteArray{body.data(), static_cast<qsizetype>(body.size())};
}

} // namespace

MusicBrainzLookupService engineLookupService(std::shared_ptr<engine::RemoteFileWork> work,
                                             QObject* context) {
    return MusicBrainzLookupService{
        .fetch =
            [work, context](const QString& url,
                            std::function<void(core::Result<QByteArray>)> completion) {
                answered_on<QByteArray>(
                    context,
                    [work, url = url.toStdString()]() -> core::Result<QByteArray> {
                        auto body = work->fetch(url, {});
                        return body ? core::Result<QByteArray>{bytes_of(*body)}
                                    : std::unexpected(std::move(body.error()));
                    },
                    std::move(completion));
            },
        .fingerprint =
            [work, context](const QString& file_path,
                            std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                // The engine's path, as the list names the file.
                const auto encoded = QFile::encodeName(file_path);
                answered_on<AcoustIdFingerprint>(
                    context,
                    [work, raw_path = std::string{encoded.constData(),
                                                  static_cast<std::size_t>(encoded.size())}]()
                        -> core::Result<AcoustIdFingerprint> {
                        auto printed = work->fingerprint(raw_path, {});
                        if (!printed) {
                            return std::unexpected(std::move(printed.error()));
                        }
                        return AcoustIdFingerprint{
                            .duration_seconds = printed->duration_seconds,
                            .fingerprint = QString::fromStdString(printed->fingerprint)};
                    },
                    std::move(completion));
            },
        .acoustid_lookup =
            [work, context](const AcoustIdFingerprint& fingerprint,
                            std::function<void(core::Result<QByteArray>)> completion) {
                answered_on<QByteArray>(
                    context,
                    [work, printed =
                               engine::MetadataServices::Fingerprint{
                                   .duration_seconds = fingerprint.duration_seconds,
                                   .fingerprint = fingerprint.fingerprint.toStdString()}]()
                        -> core::Result<QByteArray> {
                        auto body = work->acoustid_lookup(printed, {});
                        return body ? core::Result<QByteArray>{bytes_of(*body)}
                                    : std::unexpected(std::move(body.error()));
                    },
                    std::move(completion));
            },
    };
}

} // namespace trackknife::bench
