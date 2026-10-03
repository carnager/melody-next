// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/tab_store.hpp"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <set>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr auto state_key = "tabs/v1";
// The cache's own format; another is read as no cache.
constexpr quint32 cache_magic = 0x544b5443U; // "TKTC"
constexpr quint32 cache_version = 1U;

[[nodiscard]] QByteArray bytes(const std::string& text) {
    return QByteArray{text.data(), static_cast<qsizetype>(text.size())};
}

[[nodiscard]] std::string text(const QByteArray& bytes) {
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

void write(QDataStream& out, const std::string& value) { out << bytes(value); }

void write(QDataStream& out, const std::optional<std::string>& value) {
    out << value.has_value();
    if (value) {
        write(out, *value);
    }
}

void write(QDataStream& out, const std::optional<std::int64_t>& value) {
    out << value.has_value() << static_cast<qint64>(value.value_or(0));
}

void write(QDataStream& out, const std::optional<int>& value) {
    out << value.has_value() << static_cast<qint32>(value.value_or(0));
}

void read(QDataStream& in, std::string& value) {
    QByteArray raw;
    in >> raw;
    value = text(raw);
}

void read(QDataStream& in, std::optional<std::string>& value) {
    bool present = false;
    in >> present;
    if (present) {
        std::string held;
        read(in, held);
        value = std::move(held);
    } else {
        value.reset();
    }
}

void read(QDataStream& in, std::optional<std::int64_t>& value) {
    bool present = false;
    qint64 held = 0;
    in >> present >> held;
    value = present ? std::optional<std::int64_t>{held} : std::nullopt;
}

void read(QDataStream& in, std::optional<int>& value) {
    bool present = false;
    qint32 held = 0;
    in >> present >> held;
    value = present ? std::optional<int>{held} : std::nullopt;
}

void writeItem(QDataStream& out, const persistence::ListItem& item) {
    write(out, item.entry_id.to_string());
    write(out, item.source_reference);
    write(out, item.logical_reference);
    out << item.segment.has_value();
    if (item.segment) {
        out << static_cast<qint64>(item.segment->start_sample);
        write(out, item.segment->end_sample);
    }
    out << item.source_selection.has_value();
    if (item.source_selection) {
        write(out, item.source_selection->audio_stream_index);
        write(out, item.source_selection->subsong_index);
    }
    write(out, item.duration_ms);
    out << item.source_revision.has_value();
    if (item.source_revision) {
        const auto& revision = *item.source_revision;
        out << static_cast<quint64>(revision.device) << static_cast<quint64>(revision.inode)
            << static_cast<quint64>(revision.size)
            << static_cast<qint64>(revision.modification_time_seconds)
            << static_cast<qint64>(revision.modification_time_nanoseconds);
    }
    out << static_cast<quint32>(item.fields.size());
    for (const auto& field : item.fields) {
        write(out, field.name);
        write(out, field.value);
        write(out, field.native_name);
        out << static_cast<quint8>(field.provenance);
        write(out, field.language);
        write(out, field.description);
    }
}

[[nodiscard]] bool readItem(QDataStream& in, persistence::ListItem& item) {
    std::string entry;
    read(in, entry);
    const auto id = core::StableId::parse(entry);
    if (!id) {
        return false;
    }
    item.entry_id = *id;
    item.source = persistence::ListSource::local;
    read(in, item.source_reference);
    read(in, item.logical_reference);
    bool present = false;
    in >> present;
    if (present) {
        qint64 start = 0;
        std::optional<std::int64_t> end;
        in >> start;
        read(in, end);
        item.segment = persistence::ListItemSegment{.start_sample = start, .end_sample = end};
    }
    in >> present;
    if (present) {
        persistence::ListItemSourceSelection selection;
        read(in, selection.audio_stream_index);
        read(in, selection.subsong_index);
        item.source_selection = selection;
    }
    read(in, item.duration_ms);
    in >> present;
    if (present) {
        quint64 device = 0;
        quint64 inode = 0;
        quint64 size = 0;
        qint64 seconds = 0;
        qint64 nanoseconds = 0;
        in >> device >> inode >> size >> seconds >> nanoseconds;
        item.source_revision = core::LocalSourceRevision{.device = device,
                                                         .inode = inode,
                                                         .size = size,
                                                         .modification_time_seconds = seconds,
                                                         .modification_time_nanoseconds =
                                                             nanoseconds};
    }
    quint32 fields = 0;
    in >> fields;
    item.fields.reserve(std::min<quint32>(fields, 4'096U));
    for (quint32 index = 0; index < fields && in.status() == QDataStream::Ok; ++index) {
        persistence::SnapshotField field;
        read(in, field.name);
        read(in, field.value);
        read(in, field.native_name);
        quint8 provenance = 0;
        in >> provenance;
        if (provenance > static_cast<quint8>(metadata::FieldProvenance::sidecar)) {
            return false;
        }
        field.provenance = static_cast<metadata::FieldProvenance>(provenance);
        read(in, field.language);
        read(in, field.description);
        item.fields.push_back(std::move(field));
    }
    return in.status() == QDataStream::Ok;
}

} // namespace

TabStore::TabStore(QString cache_directory)
    : directory_(cache_directory.isEmpty()
                     ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                           QStringLiteral("/tabs")
                     : std::move(cache_directory)),
      writer_([this] { work(); }) {}

TabStore::~TabStore() {
    {
        const std::lock_guard guard{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    writer_.join();
}

bool TabStore::hasState() { return QSettings{}.contains(QLatin1String(state_key)); }

TabStore::State TabStore::loadState() {
    State state;
    const auto document =
        QJsonDocument::fromJson(QSettings{}.value(QLatin1String(state_key)).toByteArray());
    const auto root = document.object();
    for (const auto& value : root.value(QStringLiteral("tabs")).toArray()) {
        const auto tab = value.toObject();
        const auto id = core::StableId::parse(tab.value(QStringLiteral("id")).toString().toStdString());
        if (!id) {
            continue;
        }
        const auto kind = tab.value(QStringLiteral("kind")).toString();
        state.tabs.push_back(Tab{
            .id = *id,
            .engine = tab.value(QStringLiteral("engine")).toString().toStdString(),
            .name = text(QByteArray::fromBase64(tab.value(QStringLiteral("name")).toString().toLatin1())),
            .kind = kind == QStringLiteral("saved") ? persistence::ListKind::saved
                                                    : persistence::ListKind::scratch,
            .pinned = tab.value(QStringLiteral("pinned")).toBool(),
            .dirty = tab.value(QStringLiteral("dirty")).toBool(),
            .layout = QByteArray::fromBase64(tab.value(QStringLiteral("layout")).toString().toLatin1()),
        });
    }
    if (const auto active = core::StableId::parse(
            root.value(QStringLiteral("active")).toString().toStdString())) {
        state.active = *active;
    }
    return state;
}

void TabStore::saveState(const State& state) {
    QJsonArray tabs;
    for (const auto& tab : state.tabs) {
        tabs.append(QJsonObject{
            {QStringLiteral("id"), QString::fromStdString(tab.id.to_string())},
            {QStringLiteral("engine"), QString::fromStdString(tab.engine)},
            {QStringLiteral("name"), QString::fromLatin1(bytes(tab.name).toBase64())},
            {QStringLiteral("kind"), tab.kind == persistence::ListKind::saved
                                         ? QStringLiteral("saved")
                                         : QStringLiteral("working")},
            {QStringLiteral("pinned"), tab.pinned},
            {QStringLiteral("dirty"), tab.dirty},
            {QStringLiteral("layout"), QString::fromLatin1(tab.layout.toBase64())},
        });
    }
    QJsonObject root{{QStringLiteral("tabs"), tabs}};
    if (state.active) {
        root.insert(QStringLiteral("active"), QString::fromStdString(state.active->to_string()));
    }
    QSettings{}.setValue(QLatin1String(state_key), QJsonDocument{root}.toJson(QJsonDocument::Compact));
}

QString TabStore::pathOf(const core::StableId& id) const {
    return directory_ + QLatin1Char('/') + QString::fromStdString(id.to_string()) +
           QStringLiteral(".tabcache");
}

void TabStore::writeRows(persistence::ListDocument document) {
    {
        const std::lock_guard guard{mutex_};
        pending_[document.id.to_string()] = std::move(document);
    }
    wake_.notify_all();
}

void TabStore::work() {
    std::unique_lock lock{mutex_};
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (pending_.empty()) {
            if (stopping_) {
                return;
            }
            continue;
        }
        auto next = pending_.extract(pending_.begin());
        writing_ = true;
        lock.unlock();
        const auto& document = next.mapped();
        QDir{}.mkpath(directory_);
        QSaveFile file{pathOf(document.id)};
        if (file.open(QIODevice::WriteOnly)) {
            QDataStream out{&file};
            out.setVersion(QDataStream::Qt_6_0);
            out << cache_magic << cache_version;
            write(out, document.engine);
            write(out, document.name);
            out << static_cast<quint8>(document.kind) << document.pinned << document.dirty;
            out << static_cast<quint32>(document.items.size());
            for (const auto& item : document.items) {
                writeItem(out, item);
            }
            if (out.status() == QDataStream::Ok) {
                static_cast<void>(file.commit());
            } else {
                file.cancelWriting();
            }
        }
        lock.lock();
        writing_ = false;
        if (pending_.empty()) {
            idle_.notify_all();
        }
    }
}

std::optional<persistence::ListDocument> TabStore::readRows(const core::StableId& id) const {
    QFile file{pathOf(id)};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QDataStream in{&file};
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    in >> magic >> version;
    if (magic != cache_magic || version != cache_version) {
        return std::nullopt;
    }
    persistence::ListDocument document;
    document.id = id;
    read(in, document.engine);
    read(in, document.name);
    quint8 kind = 0;
    in >> kind >> document.pinned >> document.dirty;
    if (kind > static_cast<quint8>(persistence::ListKind::saved)) {
        return std::nullopt;
    }
    document.kind = static_cast<persistence::ListKind>(kind);
    quint32 count = 0;
    in >> count;
    document.items.reserve(std::min<quint32>(count, 1'000'000U));
    for (quint32 index = 0; index < count; ++index) {
        persistence::ListItem item;
        if (!readItem(in, item)) {
            return std::nullopt;
        }
        document.items.push_back(std::move(item));
    }
    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }
    return document;
}

void TabStore::dropAllBut(const std::vector<core::StableId>& keep) {
    std::set<QString> kept;
    for (const auto& id : keep) {
        kept.insert(QFileInfo{pathOf(id)}.fileName());
    }
    flush();
    const QDir directory{directory_};
    for (const auto& name : directory.entryList({QStringLiteral("*.tabcache")}, QDir::Files)) {
        if (!kept.contains(name)) {
            QFile::remove(directory.filePath(name));
        }
    }
}

void TabStore::flush() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return pending_.empty() && !writing_; });
}

} // namespace trackknife::bench
