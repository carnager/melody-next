// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_artwork.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QHash>
#include <QUuid>

#include <utility>

namespace trackknife::quick {

namespace {

// The open editors' artwork, for the image provider to find by id.
[[nodiscard]] QHash<QString, QPointer<QuickArtwork>>& registry() {
    static QHash<QString, QPointer<QuickArtwork>> artworks;
    return artworks;
}

} // namespace

QuickArtwork::QuickArtwork(bench::ArtworkSession* session, QObject* parent)
    : QObject(parent), session_(session), id_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    registry().insert(id_, this);
    // The tables' pictures and struck-out rows, by name.
    const QHash<int, QByteArray> roles{{Qt::DisplayRole, "display"},
                                       {Qt::ToolTipRole, "toolTip"},
                                       {Qt::DecorationRole, "decoration"},
                                       {Qt::FontRole, "font"}};
    session_->items()->setItemRoleNames(roles);
    session_->pending()->setItemRoleNames(roles);
    session_->issues()->setItemRoleNames(roles);
    connect(session_, &bench::ArtworkSession::changed, this, [this] {
        ++revision_;
        emit changed();
    });
    connect(session_, &bench::ArtworkSession::frontCoverChanged, this, [this] {
        ++revision_;
        emit changed();
    });
    connect(session_, &bench::ArtworkSession::pickerChanged, this, &QuickArtwork::pickerChanged);
    connect(session_, &bench::ArtworkSession::pickerClosed, this, &QuickArtwork::pickerClosed);
    connect(session_, &bench::ArtworkSession::feedbackRequested, this,
            [this](const QString& title, const QString& summary,
                   std::vector<bench::PreparationFeedbackRow> rows) {
                QVariantList list;
                for (const auto& row : rows) {
                    list.push_back(QVariantMap{{QStringLiteral("file"), row.file},
                                               {QStringLiteral("detail"), row.detail}});
                }
                emit feedbackRequested(title, summary, list);
            });
    connect(session_, &bench::ArtworkSession::folderImagesReviewRequested, this,
            [this](std::vector<metadata::FolderImageWritePlan> images) {
                QVariantList rows;
                for (const auto& cells : bench::folderImageReviewRows(images)) {
                    rows.push_back(cells);
                }
                emit folderImagesRequested(QString::fromLatin1(bench::folder_image_review_note),
                                           rows);
            });
}

QuickArtwork::~QuickArtwork() { registry().remove(id_); }

QPointer<QuickArtwork> QuickArtwork::find(const QString& id) { return registry().value(id); }

QVariantMap QuickArtwork::state() const {
    const auto& session = *session_;
    return {
        {QStringLiteral("status"), session.status()},
        {QStringLiteral("emptyText"), session.emptyText()},
        {QStringLiteral("emptyVisible"), session.emptyVisible()},
        {QStringLiteral("issuesVisible"), session.issuesVisible()},
        {QStringLiteral("pendingVisible"), session.pendingVisible()},
        {QStringLiteral("draftHelp"), session.draftHelp()},
        {QStringLiteral("saveVisible"), session.saveVisible()},
        {QStringLiteral("working"), session.working()},
        {QStringLiteral("progressVisible"), session.progressVisible()},
        {QStringLiteral("progress"), session.progressValue()},
        {QStringLiteral("progressMaximum"), session.progressMaximum()},
        {QStringLiteral("canStop"), session.canStop()},
        {QStringLiteral("canFetch"), session.canFetch()},
        {QStringLiteral("canAdd"), session.canAdd()},
        {QStringLiteral("canCopy"), session.canCopy()},
        {QStringLiteral("canExport"), session.canExport()},
        {QStringLiteral("canReplace"), session.canReplace()},
        {QStringLiteral("canRemove"), session.canRemove()},
        {QStringLiteral("canSave"), session.canSave()},
        {QStringLiteral("canDiscard"), session.canDiscard()},
        {QStringLiteral("canUndoSelected"), session.canUndoSelected()},
        {QStringLiteral("frontEditable"), session.frontEditable()},
        {QStringLiteral("canRemoveFront"), session.canRemoveFront()},
        {QStringLiteral("frontMixed"), session.frontMixed()},
        {QStringLiteral("hasFront"), !session.frontImage().isNull()},
        {QStringLiteral("pickerOpen"), session.pickerOpen()},
    };
}

QVariantList QuickArtwork::pickerRows() const {
    QVariantList rows;
    for (const auto& row : session_->pickerRows()) {
        rows.push_back(QVariantMap{{QStringLiteral("from"), row.from},
                                   {QStringLiteral("type"), row.type},
                                   {QStringLiteral("details"), row.details},
                                   {QStringLiteral("toolTip"), row.tool_tip},
                                   {QStringLiteral("hasThumbnail"), !row.thumbnail.isNull()}});
    }
    return rows;
}

void QuickArtwork::stageFrontCover(const QUrl& file) {
    if (file.isLocalFile()) {
        session_->stageFrontCover(file.toLocalFile());
    }
}

void QuickArtwork::pasteFrontCover() {
    if (const auto* clipboard = QGuiApplication::clipboard()) {
        session_->pasteFrontCover(clipboard->image());
    }
}

QImage QuickArtwork::image(const QString& what) const {
    const auto parts = what.split(QLatin1Char('/'));
    if (parts.value(0) == QStringLiteral("front")) {
        return session_->frontImage();
    }
    if (parts.value(0) == QStringLiteral("picker")) {
        const auto row = parts.value(1).toInt();
        const auto& rows = session_->pickerRows();
        return row >= 0 && static_cast<std::size_t>(row) < rows.size()
                   ? rows[static_cast<std::size_t>(row)].thumbnail
                   : QImage{};
    }
    const auto* model = parts.value(0) == QStringLiteral("items")     ? session_->items()
                        : parts.value(0) == QStringLiteral("pending") ? session_->pending()
                                                                      : nullptr;
    if (model == nullptr) {
        return {};
    }
    return model->index(parts.value(1).toInt(), parts.value(2).toInt())
        .data(Qt::DecorationRole)
        .value<QImage>();
}

ArtworkImageProvider::ArtworkImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ArtworkImageProvider::requestImage(const QString& id, QSize* size, const QSize& requested) {
    const auto path = id.section(QLatin1Char('?'), 0, 0);
    const auto artwork = QuickArtwork::find(path.section(QLatin1Char('/'), 0, 0));
    auto image = artwork ? artwork->image(path.section(QLatin1Char('/'), 1)) : QImage{};
    if (image.isNull()) {
        image = QImage{1, 1, QImage::Format_ARGB32};
        image.fill(Qt::transparent);
    } else if (requested.isValid() && requested.width() > 0 && requested.height() > 0) {
        image = image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

} // namespace trackknife::quick
