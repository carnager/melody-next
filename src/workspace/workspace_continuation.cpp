// SPDX-License-Identifier: GPL-3.0-only

// ADR-0253: a list continues with a dynamic playlist's rule. The engine keeps
// a copy of the rule and does the continuing; the window chooses it and keeps
// the copies in step with the definitions.

#include "workspace/workspace.hpp"

#include "bench/dynamic_playlist_service.hpp"
#include "workspace/workspace_view.hpp"

namespace trackknife::bench {
namespace {

const QString definitions_profile = QStringLiteral("local");

// A list can continue only with a tkq-1 rule, which the engine runs itself:
// a Last.fm source would stop with the window.
[[nodiscard]] bool continuable(const DynamicPlaylistDefinition& definition) {
    return definition.source == QStringLiteral("rules") && !definition.id.isEmpty() &&
           !definition.query.trimmed().isEmpty();
}

[[nodiscard]] QHash<QString, EnginePlayback::Continuation> continuableRules() {
    QHash<QString, EnginePlayback::Continuation> rules;
    const auto definitions = dynamicPlaylistCatalog(definitions_profile);
    if (!definitions) {
        return rules;
    }
    for (const auto& definition : *definitions) {
        if (continuable(definition)) {
            const auto selection = definition.selection();
            rules.insert(definition.id,
                         EnginePlayback::Continuation{
                             .rule_id = definition.id,
                             .name = definition.name,
                             .query = definition.query,
                             .group_by = QString::fromStdString(selection.group_by),
                             .groups = static_cast<int>(selection.groups),
                             .per_group = static_cast<int>(selection.per_group),
                             .limit = static_cast<int>(selection.limit),
                             .shuffle = selection.shuffle});
        }
    }
    return rules;
}

} // namespace

std::vector<Workspace::ContinuationChoice> Workspace::continuationChoices() const {
    std::vector<ContinuationChoice> choices;
    const auto definitions = dynamicPlaylistCatalog(definitions_profile);
    if (!definitions) {
        return choices;
    }
    for (const auto& definition : *definitions) {
        if (continuable(definition)) {
            choices.push_back({.rule_id = definition.id,
                               .name = definition.name,
                               .shipped = definition.shipped()});
        }
    }
    return choices;
}

std::optional<EnginePlayback::Continuation> Workspace::continuationOf(const ListTab& tab) const {
    const auto* playback = playbackOf(EngineKey::of(tab.document));
    if (playback == nullptr) {
        return std::nullopt;
    }
    const auto found = playback->continuations().constFind(document_text(tab.document.id));
    if (found == playback->continuations().constEnd()) {
        return std::nullopt;
    }
    return *found;
}

void Workspace::setContinuation(const ListTab& tab, const QString& rule_id) {
    auto* playback = playbackOf(EngineKey::of(tab.document));
    if (playback == nullptr || !playback->active()) {
        view_->showMessage(tr("The list's engine is not connected"), 5'000);
        return;
    }
    std::optional<EnginePlayback::Continuation> rule;
    if (!rule_id.isEmpty()) {
        const auto rules = continuableRules();
        const auto found = rules.constFind(rule_id);
        if (found == rules.constEnd()) {
            view_->showMessage(tr("That dynamic playlist is gone"), 5'000);
            return;
        }
        rule = *found;
    }
    playback->setContinuation(document_text(tab.document.id), rule);
}

void Workspace::rememberContinuationRules() {
    QSet<QString> known;
    for (const auto& rule : continuableRules()) {
        known.insert(rule.rule_id);
    }
    known_rules_ = std::move(known);
}

void Workspace::refreshContinuations() {
    if (!known_rules_) {
        rememberContinuationRules();
    }
    // Against the rules known before the save, so a deleted one is told.
    for (const auto& engine : engines_) {
        syncContinuations(*engine);
    }
    rememberContinuationRules();
}

void Workspace::followContinuations(EngineLink& engine) {
    // A rule edited while the engine was away is brought in step as it is
    // back: connecting asks for its continuations.
    connect(engine.playback, &EnginePlayback::continuationsChanged, this, [this, link = &engine] {
        syncContinuations(*link);
        for (const auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == link->key) {
                view_->refreshTabChrome(*tab);
            }
        }
    });
}

void Workspace::syncContinuations(EngineLink& engine) {
    if (engine.playback == nullptr || !engine.playback->active() ||
        engine.playback->continuations().isEmpty()) {
        return;
    }
    if (!known_rules_) {
        rememberContinuationRules();
    }
    const auto rules = continuableRules();
    // Copied, as setting one is answered with a new set.
    const auto continuations = engine.playback->continuations();
    for (auto entry = continuations.constBegin(); entry != continuations.constEnd(); ++entry) {
        const auto found = rules.constFind(entry->rule_id);
        if (found != rules.constEnd()) {
            if (!found->sameAs(*entry)) {
                engine.playback->setContinuation(entry.key(), *found);
            }
        } else if (known_rules_->contains(entry->rule_id)) {
            // A rule this window had, deleted or no longer tkq-1. One it never
            // knew is another window's to keep.
            engine.playback->setContinuation(entry.key(), std::nullopt);
        }
    }
}

} // namespace trackknife::bench
