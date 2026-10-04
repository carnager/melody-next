// SPDX-License-Identifier: GPL-3.0-only
#include "bench/single_instance.hpp"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#include <utility>

namespace trackknife::bench {
namespace {

// What a start hands over: one base64 raw path a line, then an empty line.
[[nodiscard]] QByteArray message_of(const std::vector<std::string>& raw_paths) {
    QByteArray message;
    for (const auto& path : raw_paths) {
        message += QByteArray{path.data(), static_cast<qsizetype>(path.size())}.toBase64();
        message += '\n';
    }
    message += '\n';
    return message;
}

constexpr int wait_ms = 3'000;

} // namespace

SingleInstance::SingleInstance(QString key, QObject* parent) : QObject(parent) {
    const auto hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256)
                          .toHex()
                          .left(16);
    name_ = QStringLiteral("trackknife-%1").arg(QString::fromLatin1(hash));
    auto folder = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (folder.isEmpty()) {
        folder = QDir::tempPath();
    }
    lock_ = std::make_unique<QLockFile>(QDir{folder}.filePath(name_ + QStringLiteral(".lock")));
    // Never stale while the holder lives: QLockFile asks whether its process
    // does; a crashed one's lock is taken over.
    lock_->setStaleLockTime(0);
}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::defaultKey() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

bool SingleInstance::claim(const std::vector<std::string>& raw_paths) {
    const QDeadlineTimer deadline{wait_ms};
    while (true) {
        if (lock_->tryLock(0)) {
            listen();
            return true;
        }
        // Held: the one running takes this start's files -- once it listens,
        // for it may be starting too.
        if (handOver(raw_paths)) {
            return false;
        }
        if (deadline.hasExpired()) {
            // Held, but no one answers: run rather than refuse to start.
            return true;
        }
        QThread::msleep(100);
    }
}

bool SingleInstance::handOver(const std::vector<std::string>& raw_paths) {
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (!socket.waitForConnected(500)) {
        return false;
    }
    socket.write(message_of(raw_paths));
    if (!socket.waitForBytesWritten(wait_ms)) {
        return false;
    }
    // Answered once it has read them.
    return socket.waitForReadyRead(wait_ms) && socket.readAll().startsWith("ok");
}

void SingleInstance::listen() {
    // A socket a crash left behind goes; the lock says none is running.
    QLocalServer::removeServer(name_);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name_)) {
        return;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (auto* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                const auto buffered = socket->peek(socket->bytesAvailable());
                if (!buffered.endsWith("\n\n") && buffered != "\n") {
                    return;
                }
                std::vector<std::string> raw_paths;
                for (const auto& line : socket->readAll().split('\n')) {
                    if (!line.isEmpty()) {
                        const auto path = QByteArray::fromBase64(line);
                        raw_paths.emplace_back(path.constData(),
                                               static_cast<std::size_t>(path.size()));
                    }
                }
                socket->write("ok\n");
                socket->flush();
                emit asked(std::move(raw_paths));
            });
        }
    });
}

} // namespace trackknife::bench
