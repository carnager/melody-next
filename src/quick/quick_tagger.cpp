// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_tagger.hpp"

#include "bench/metadata_grid_model.hpp"
#include "workspace/workspace.hpp"

#include <QBrush>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>

#include <algorithm>
#include <utility>

namespace trackknife::quick {

QHash<int, QByteArray> TaggerFieldRows::roleNames() const {
    auto names = QIdentityProxyModel::roleNames();
    names.insert(foreground_role, "foreground");
    names.insert(bold_role, "bold");
    names.insert(italic_role, "italic");
    names.insert(strike_role, "strike");
    names.insert(staged_role, "staged");
    names.insert(canonical_role, "canonical");
    names.insert(editable_role, "editable");
    names.insert(placeholder_role, "placeholder");
    return names;
}

QVariant TaggerFieldRows::data(const QModelIndex& index, const int role) const {
    switch (role) {
    case foreground_role: {
        const auto brush = QIdentityProxyModel::data(index, Qt::ForegroundRole);
        return brush.canConvert<QBrush>() ? QVariant{brush.value<QBrush>().color()} : QVariant{};
    }
    case bold_role:
    case italic_role:
    case strike_role: {
        const auto font = QIdentityProxyModel::data(index, Qt::FontRole);
        if (!font.canConvert<QFont>()) {
            return false;
        }
        const auto value = font.value<QFont>();
        return role == bold_role     ? value.bold()
               : role == italic_role ? value.italic()
                                     : value.strikeOut();
    }
    case staged_role:
        return QIdentityProxyModel::data(index, bench::metadata_cell_staged_role).toBool();
    case canonical_role:
        return QIdentityProxyModel::data(index, bench::metadata_field_canonical_name_role);
    case editable_role:
        return flags(index).testFlag(Qt::ItemIsEditable);
    case placeholder_role: {
        // Greyed: "(various …)", "(preparing …)" -- the palette's own
        // placeholder colour, as the widgets delegate draws it.
        const auto brush = QIdentityProxyModel::data(index, Qt::ForegroundRole);
        return brush.canConvert<QBrush>() &&
               brush.value<QBrush>().color() ==
                   QGuiApplication::palette().color(QPalette::PlaceholderText);
    }
    default:
        return QIdentityProxyModel::data(index, role);
    }
}

QuickTagger::QuickTagger(QString title, const std::size_t count,
                         bench::MetadataPropertiesSourceReader reader,
                         bench::TaggerServices services,
                         bench::ArtworkWritePlanApplierFactory artwork_applier,
                         bench::ArtworkApplyObserver artwork_observer, QObject* parent)
    : QObject(parent), title_(std::move(title)) {
    const auto tools = services.tools;
    const auto unified = static_cast<bool>(services.plan_applier_factory);
    session_ = new bench::TaggerSession(count, std::move(reader), bench::Workspace::taggerFields(),
                                        std::move(services), this);
    session_->setArtworkServices(std::move(artwork_applier), std::move(artwork_observer));
    // The covers, as the widgets editor's Artwork page has them: read while
    // the editor is open (its Fields page shows the front cover too), saved
    // with the tags' Apply.
    artwork_session_ = new bench::ArtworkSession(this);
    artwork_session_->setFileWorkTools(tools);
    artwork_session_->setUnifiedApply(unified);
    artwork_session_->setMutationServices(session_->artworkApplierFactory(),
                                          session_->artworkAppliedObserver());
    if (auto service = session_->coverArtService()) {
        artwork_session_->setCoverArtService(std::move(*service));
    }
    artwork_session_->setActive(true);
    connect(artwork_session_, &bench::ArtworkSession::operationRunningChanged, session_,
            &bench::TaggerSession::setArtworkOperationRunning);
    connect(artwork_session_, &bench::ArtworkSession::pendingChangesChanged, session_,
            &bench::TaggerSession::artworkStateChanged);
    session_->setArtwork(artwork_session_);
    artwork_ = new QuickArtwork(artwork_session_, this);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(40);
    connect(filter_debounce_, &QTimer::timeout, this, &QuickTagger::refilter);
    connect(session_, &bench::TaggerSession::changed, this, &QuickTagger::changed);
    connect(session_, &bench::TaggerSession::gridReady, this, [this] {
        auto* aggregate = session_->aggregateModel();
        field_rows_ = new TaggerFieldRows(this);
        field_rows_->setSourceModel(aggregate);
        field_selection_ = new QItemSelectionModel(field_rows_, this);
        connect(field_selection_, &QItemSelectionModel::selectionChanged, this,
                &QuickTagger::noteFieldSelection);
        connect(field_selection_, &QItemSelectionModel::currentChanged, this,
                &QuickTagger::noteFieldSelection);
        const auto schedule = [this] { filter_debounce_->start(); };
        connect(aggregate, &QAbstractItemModel::dataChanged, this, schedule);
        connect(aggregate, &QAbstractItemModel::rowsInserted, this, schedule);
        connect(aggregate, &QAbstractItemModel::modelReset, this, schedule);
        connect(aggregate, &bench::MetadataAggregateModel::selectionProjectionChanged, this,
                schedule);
        connect(aggregate, &bench::MetadataAggregateModel::draftProjectionChanged, this, schedule);
        auto* grid = session_->gridModel();
        connect(grid, &QAbstractItemModel::modelReset, this, &QuickTagger::filesChanged);
        connect(grid, &QAbstractItemModel::rowsInserted, this, &QuickTagger::filesChanged);
        connect(grid, &QAbstractItemModel::rowsRemoved, this, &QuickTagger::filesChanged);
        connect(session_->fileSelection(), &QItemSelectionModel::selectionChanged, this, [this] {
            ++file_selection_revision_;
            emit fileSelectionChanged();
        });
        refilter();
        emit gridReady();
        emit filesChanged();
    });
    connect(session_, &bench::TaggerSession::gridFilled, this, [this] {
        if (field_selection_ != nullptr && session_->aggregateModel()->rowCount() > 0) {
            field_selection_->setCurrentIndex(field_rows_->index(0, 2),
                                              QItemSelectionModel::NoUpdate);
        }
        noteFieldSelection();
    });
    connect(session_, &bench::TaggerSession::scriptsChanged, this, &QuickTagger::scriptsChanged);
    connect(session_, &bench::TaggerSession::fieldLayoutsChanged, this, [this] {
        filter_.layout_fields = session_->activeFieldLayoutFields();
        refilter();
        emit fieldLayoutsChanged();
    });
    connect(session_, &bench::TaggerSession::outputProfilesChanged, this,
            &QuickTagger::outputProfilesChanged);
    connect(session_, &bench::TaggerSession::feedbackRequested, this,
            [this](const QString& title, const QString& summary,
                   std::vector<bench::PreparationFeedbackRow> rows, const bool retry_offered) {
                QVariantList list;
                for (const auto& row : rows) {
                    list.push_back(QVariantMap{{QStringLiteral("file"), row.file},
                                               {QStringLiteral("detail"), row.detail}});
                }
                emit feedbackRequested(title, summary, list, retry_offered,
                                       session_->applyCommitted());
            });
    connect(session_, &bench::TaggerSession::folderImagesReviewRequested, this,
            [this](std::vector<metadata::FolderImageWritePlan> images) {
                QVariantList rows;
                for (const auto& cells : bench::folderImageReviewRows(images)) {
                    rows.push_back(cells);
                }
                emit folderImagesRequested(QString::fromLatin1(bench::folder_image_review_note),
                                           rows);
            });
    connect(session_, &bench::TaggerSession::closeRequested, this, &QuickTagger::closeRequested);
    connect(session_, &bench::TaggerSession::openSettingsRequested, this,
            [this](const bench::TaggerSession::SettingsPage page) {
                switch (page) {
                case bench::TaggerSession::SettingsPage::naming:
                    emit settingsRequested(QStringLiteral("naming"));
                    break;
                case bench::TaggerSession::SettingsPage::covers:
                    emit settingsRequested(QStringLiteral("covers"));
                    break;
                case bench::TaggerSession::SettingsPage::replaygain:
                    emit settingsRequested(QStringLiteral("replaygain"));
                    break;
                }
            });
    connect(session_, &bench::TaggerSession::openDestinationsRequested, this,
            &QuickTagger::destinationsRequested);
    connect(session_, &bench::TaggerSession::statusMessage, this, &QuickTagger::statusMessage);
    session_->start();
}

QuickTagger::~QuickTagger() {
    session_->setArtwork(nullptr);
    delete session_;
    session_ = nullptr;
}

QVariantMap QuickTagger::state() const {
    const auto& session = *session_;
    return {
        {QStringLiteral("loadingText"), session.loadingText()},
        {QStringLiteral("summary"), session.summary()},
        {QStringLiteral("status"), session.status()},
        {QStringLiteral("statusRich"), session.statusRich()},
        {QStringLiteral("technical"), session.technical()},
        {QStringLiteral("applySummary"), session.applySummary()},
        {QStringLiteral("draftCount"), session.draftCount()},
        {QStringLiteral("canUndo"), session.canUndo()},
        {QStringLiteral("canRedo"), session.canRedo()},
        {QStringLiteral("canAddField"), session.canAddField()},
        {QStringLiteral("canRemoveFields"), session.canRemoveFields()},
        {QStringLiteral("canEditValues"), session.canEditValues()},
        {QStringLiteral("canTransform"), session.canTransform()},
        {QStringLiteral("canSuggest"), session.canSuggest()},
        {QStringLiteral("canIdentify"), session.canIdentify()},
        {QStringLiteral("canScan"), session.canScanReplayGain()},
        {QStringLiteral("canShowSources"), session.canShowProvenance()},
        {QStringLiteral("canApply"), session.canApply()},
        {QStringLiteral("applying"), session.progressVisible()},
        {QStringLiteral("progress"), session.progressValue()},
        {QStringLiteral("progressMaximum"), session.progressMaximum()},
        {QStringLiteral("canStop"), session.canStopApply()},
        {QStringLiteral("filesEnabled"), session.fileListEnabled()},
        {QStringLiteral("saveTags"), session.saveTags()},
        {QStringLiteral("renameFiles"), session.renameFiles()},
        {QStringLiteral("moveFiles"), session.moveFiles()},
        {QStringLiteral("renameAvailable"), session.renameAvailable()},
        {QStringLiteral("moveAvailable"), session.moveAvailable()},
        {QStringLiteral("renameTooltip"), session.renameTooltip()},
        {QStringLiteral("moveTooltip"), session.moveTooltip()},
        {QStringLiteral("layoutsAvailable"), session.layoutsAvailable()},
        {QStringLiteral("destinationsAvailable"), session.destinationsAvailable()},
        {QStringLiteral("layoutIndex"), session.layoutIndex()},
        {QStringLiteral("destinationIndex"), session.destinationIndex()},
        {QStringLiteral("destinationsOn"), session.destinationsOn()},
        {QStringLiteral("profileStatus"), session.outputProfileStatus()},
        {QStringLiteral("grouping"), session.replayGainGrouping()},
        {QStringLiteral("expression"), session.replayGainExpression()},
        {QStringLiteral("scriptStatus"), session.scriptStatus()},
        {QStringLiteral("scriptsLoading"), session.scriptsLoading()},
        {QStringLiteral("hasExport"), session.hasReplayGainExport()},
        {QStringLiteral("selectedFiles"), static_cast<int>(session.selectedItemCount())},
    };
}

QVariantList QuickTagger::scripts() const {
    QVariantList list;
    for (const auto& script : session_->scripts()) {
        list.push_back(QVariantMap{{QStringLiteral("id"), script.id},
                                   {QStringLiteral("name"), script.name},
                                   {QStringLiteral("automatic"), script.automatic}});
    }
    return list;
}

QVariantList QuickTagger::fieldLayouts() const {
    QVariantList list;
    for (const auto& layout : session_->fieldLayouts()) {
        list.push_back(
            QVariantMap{{QStringLiteral("id"), layout.id}, {QStringLiteral("name"), layout.name}});
    }
    return list;
}

QStringList QuickTagger::layouts() const {
    QStringList names;
    for (const auto& layout : session_->layouts()) {
        names.push_back(layout.name);
    }
    return names;
}

QStringList QuickTagger::destinations() const {
    QStringList names;
    for (const auto& destination : session_->destinations()) {
        names.push_back(destination.name);
    }
    return names;
}

// The files.

QAbstractItemModel* QuickTagger::files() const { return session_->gridModel(); }

bool QuickTagger::fileSelected(const int row) const {
    auto* grid = session_->gridModel();
    return grid != nullptr && session_->fileSelection()->isRowSelected(row);
}

QString QuickTagger::fileText(const int row) const {
    auto* grid = session_->gridModel();
    if (grid == nullptr) {
        return {};
    }
    const auto path = grid->index(row, 0).data().toString();
    const auto prefix = session_->commonFolder();
    return !prefix.isEmpty() && path.startsWith(prefix) ? path.mid(prefix.size()) : path;
}

void QuickTagger::clickFile(const int row, const int modifiers, const bool on_check) {
    auto* grid = session_->gridModel();
    if (grid == nullptr || row < 0 || row >= grid->rowCount()) {
        return;
    }
    auto* selection = session_->fileSelection();
    const auto index = grid->index(row, 0);
    const auto flags = Qt::KeyboardModifiers{modifiers};
    if (on_check || flags.testFlag(Qt::ControlModifier)) {
        selection->select(index, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
        file_anchor_ = row;
    } else if (flags.testFlag(Qt::ShiftModifier) && file_anchor_ >= 0) {
        const QItemSelection range{grid->index(std::min(row, file_anchor_), 0),
                                   grid->index(std::max(row, file_anchor_), 0)};
        selection->select(range, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    } else {
        selection->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        file_anchor_ = row;
    }
    selection->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
}

void QuickTagger::selectAllFiles() {
    auto* grid = session_->gridModel();
    if (grid == nullptr || grid->rowCount() == 0) {
        return;
    }
    session_->fileSelection()->select(
        QItemSelection{grid->index(0, 0), grid->index(grid->rowCount() - 1, 0)},
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

// The fields.

void QuickTagger::setFilterText(const QString& text) {
    if (filter_.query == text) {
        return;
    }
    filter_.query = text;
    filter_debounce_->start();
    emit filterChanged();
}

void QuickTagger::setChangedOnly(const bool on) {
    if (filter_.changed_only == on) {
        return;
    }
    filter_.changed_only = on;
    refilter();
}

void QuickTagger::refilter() {
    auto* aggregate = session_->aggregateModel();
    if (aggregate == nullptr) {
        return;
    }
    auto outcome = bench::filterFields(*aggregate, filter_);
    filter_status_ = outcome.status;
    if (outcome.hidden) {
        hidden_ = std::move(*outcome.hidden);
        // A field that disappears must not remain an invisible Remove/Revert
        // target.
        QItemSelection hidden_selection;
        for (int row = 0; row < aggregate->rowCount(); ++row) {
            if (hidden_[static_cast<std::size_t>(row)]) {
                hidden_selection.select(field_rows_->index(row, 0), field_rows_->index(row, 2));
            }
        }
        field_selection_->select(hidden_selection, QItemSelectionModel::Deselect);
        const auto current = field_selection_->currentIndex();
        if (current.isValid() && fieldHidden(current.row())) {
            field_selection_->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
        }
    }
    ++filter_revision_;
    emit filterChanged();
}

bool QuickTagger::fieldHidden(const int row) const {
    return row >= 0 && static_cast<std::size_t>(row) < hidden_.size() &&
           hidden_[static_cast<std::size_t>(row)];
}

int QuickTagger::currentField() const {
    return field_selection_ != nullptr && field_selection_->currentIndex().isValid()
               ? field_selection_->currentIndex().row()
               : -1;
}

void QuickTagger::noteFieldSelection() {
    if (field_selection_ == nullptr) {
        return;
    }
    session_->setFieldSelection(!field_selection_->selectedRows(0).empty(),
                                field_selection_->currentIndex().isValid());
    emit fieldSelectionChanged();
}

std::vector<int> QuickTagger::selectedFieldRows() const {
    std::vector<int> rows;
    if (field_selection_ == nullptr) {
        return rows;
    }
    for (const auto& index : field_selection_->selectedRows(0)) {
        rows.push_back(index.row());
    }
    std::ranges::sort(rows);
    return rows;
}

void QuickTagger::setField(const int row, const QString& value) {
    auto* aggregate = session_->aggregateModel();
    if (aggregate != nullptr) {
        aggregate->setData(aggregate->index(row, 2), value, Qt::EditRole);
    }
}

void QuickTagger::removeFields() {
    if (field_selection_ != nullptr) {
        static_cast<void>(session_->aggregateModel()->removeIndexes(
            field_rows_->mapSelectionToSource(field_selection_->selection()).indexes()));
    }
}

void QuickTagger::revertFields() {
    if (field_selection_ != nullptr) {
        static_cast<void>(session_->aggregateModel()->revertIndexes(
            field_rows_->mapSelectionToSource(field_selection_->selection()).indexes()));
    }
}

int QuickTagger::addField(const QString& name) {
    const auto row = session_->addField(name);
    if (row < 0) {
        return row;
    }
    filter_.query.clear();
    filter_.changed_only = false;
    refilter();
    const auto index = field_rows_->index(row, 2);
    field_selection_->select(index,
                             QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    field_selection_->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    emit revealField(row);
    return row;
}

QStringList QuickTagger::fieldNameSuggestions(const QString& query) const {
    return session_->fieldNameSuggestions(query);
}

QVariantMap QuickTagger::exactValues() const {
    const auto exact = session_->exactValues(currentField());
    if (!exact) {
        return {};
    }
    return {{QStringLiteral("row"), exact->row},
            {QStringLiteral("heading"), exact->heading},
            {QStringLiteral("context"), exact->context},
            {QStringLiteral("values"), exact->values}};
}

void QuickTagger::replaceValues(const int row, const QStringList& values) {
    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& value : values) {
        const auto bytes = value.toUtf8();
        encoded.emplace_back(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    }
    session_->replaceValues(row, std::move(encoded));
}

void QuickTagger::statusLink(const QString& link) { session_->statusLinkActivated(link); }

QuickScript* QuickTagger::scriptEditor(const QString& id) {
    if (!session_->canTransform()) {
        return nullptr;
    }
    auto items = session_->selectedItems();
    if (items.empty()) {
        return nullptr;
    }
    auto* grid = session_->gridModel();
    QStringList labels;
    labels.reserve(grid->rowCount());
    for (auto row = 0; row < grid->rowCount(); ++row) {
        labels.push_back(grid->trackLabel(row));
    }
    const auto parsed = core::StableId::parse(id.toStdString());
    const auto initially = parsed ? std::optional{*parsed} : std::nullopt;
    const QPointer session{session_};
    auto* script = new QuickScript(
        new bench::ScriptSession(
            grid->sharedSelection(), grid->patches(), std::move(items), std::move(labels),
            [session](const metadata::MetadataTransformationPreview& preview) {
                return session && session->stageTransformation(preview);
            },
            session_->services().transformation_store, initially),
        this);
    // Kept by this editor, not the QML engine: its window releases it.
    QQmlEngine::setObjectOwnership(script, QQmlEngine::CppOwnership);
    session_->setTransformationDialogOpen(true);
    connect(script, &QObject::destroyed, this, [session, initially] {
        if (session) {
            session->setTransformationDialogOpen(false);
            session->reloadScripts(initially);
        }
    });
    return script;
}

QuickIdentify* QuickTagger::identify() {
    auto request = session_->identifyRequest();
    if (!request) {
        return nullptr;
    }
    auto* identify = new QuickIdentify(
        new bench::IdentifySession(session_->services().musicbrainz,
                                   std::move(request->descriptors), std::move(request->local_paths),
                                   std::move(request->items), request->artist, request->release),
        this);
    // Kept by this editor, not the QML engine: its window releases it.
    QQmlEngine::setObjectOwnership(identify, QQmlEngine::CppOwnership);
    session_->setIdentifyDialogOpen(true);
    const QPointer session{session_};
    connect(identify->findChild<bench::IdentifySession*>(), &bench::IdentifySession::accepted, this,
            [session](metadata::MetadataProposalSet proposals) {
                if (session) {
                    session->applyMusicBrainzProposals(std::move(proposals));
                }
            });
    connect(identify, &QObject::destroyed, this, [session] {
        if (session) {
            session->setIdentifyDialogOpen(false);
        }
    });
    return identify;
}

// Field sets.

void QuickTagger::selectFieldLayout(const QString& id) { session_->selectFieldLayout(id); }

void QuickTagger::saveFieldLayout(const QString& name) {
    auto* aggregate = session_->aggregateModel();
    if (aggregate == nullptr) {
        return;
    }
    QStringList fields;
    for (const auto row : selectedFieldRows()) {
        fields.push_back(
            aggregate->index(row, 0).data(bench::metadata_field_canonical_name_role).toString());
    }
    if (fields.isEmpty()) {
        for (int row = 0; row < aggregate->rowCount(); ++row) {
            if (!fieldHidden(row)) {
                fields.push_back(aggregate->index(row, 0)
                                     .data(bench::metadata_field_canonical_name_role)
                                     .toString());
            }
        }
    }
    static_cast<void>(session_->saveFieldLayout(name, std::move(fields)));
}

// What Apply does.

void QuickTagger::setSaveTags(const bool on) {
    session_->setSaveTags(on);
    session_->rememberActionChoices();
}

void QuickTagger::selectLayout(const int index) {
    session_->selectLayout(index);
    session_->rememberActionChoices();
}

void QuickTagger::selectDestination(const int index) {
    session_->selectDestination(index);
    session_->rememberActionChoices();
}

void QuickTagger::exportReplayGain(const QUrl& file) {
    session_->exportReplayGainResults(file.isLocalFile() ? file.toLocalFile() : file.toString());
}

QVariantMap QuickTagger::loudnessSources() const {
    const auto provenance = session_->loudnessProvenance();
    QVariantList rows;
    for (const auto& row : provenance.rows) {
        rows.push_back(row);
    }
    return {{QStringLiteral("headers"), provenance.headers}, {QStringLiteral("rows"), rows}};
}

// Closing.

QString QuickTagger::requestClose() {
    switch (session_->requestClose()) {
    case bench::TaggerSession::CloseAnswer::close:
        return QStringLiteral("close");
    case bench::TaggerSession::CloseAnswer::confirm_artwork:
        return QStringLiteral("confirm-artwork");
    case bench::TaggerSession::CloseAnswer::confirm_drafts:
        return QStringLiteral("confirm-drafts");
    case bench::TaggerSession::CloseAnswer::wait:
        return QStringLiteral("wait");
    }
    return QStringLiteral("close");
}

} // namespace trackknife::quick
