// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/open_list_session.hpp"

#include "workspace/workspace.hpp"

#include <QPointer>

#include <algorithm>

namespace trackknife::bench {

OpenListSession::OpenListSession(Workspace& work, QObject* parent) : QObject(parent), work_(work) {
    for (const auto& engine : work_.engines_) {
        if (engine->playback == nullptr || !engine->playback->active()) {
            continue;
        }
        groups_.push_back(
            {.name = engine->key.isLocal() ? tr("This computer") : engine->catalogue->name(),
             .engine = engine->key,
             .lists = {},
             .note = {},
             .answered = false});
    }
    for (std::size_t group = 0; group < groups_.size(); ++group) {
        const QPointer self{this};
        work_.playbackOf(groups_[group].engine)
            ->request(QStringLiteral("list.all"), protocol::Json::object(),
                      [self, group](const core::Result<protocol::Json>& answer) {
                          if (!self) {
                              return;
                          }
                          auto& shown = self->groups_[group];
                          auto lists = answer
                                           ? answer->value("lists", std::vector<protocol::Json>{})
                                           : std::vector<protocol::Json>{};
                          std::ranges::stable_partition(lists, [](const protocol::Json& list) {
                              return list.value("kind", std::string{}) == "saved";
                          });
                          shown.lists.clear();
                          for (const auto& list : lists) {
                              auto label =
                                  QString::fromStdString(list.value("name", std::string{}));
                              if (list.value("kind", std::string{}) != "saved") {
                                  label += tr(" (working)");
                              }
                              shown.lists.push_back(
                                  {.id = QString::fromStdString(list.value("id", std::string{})),
                                   .label = label,
                                   .tracks = list.value("tracks", 0)});
                          }
                          shown.note =
                              !lists.empty() ? QString{}
                              : answer       ? tr("No lists")
                                             : tr("Cannot say: %1")
                                             .arg(QString::fromStdString(answer.error().message));
                          shown.answered = true;
                          emit self->changed();
                      });
    }
}

void OpenListSession::open(const int group, const int row) {
    if (group < 0 || group >= static_cast<int>(groups_.size())) {
        return;
    }
    const auto& shown = groups_[static_cast<std::size_t>(group)];
    if (row < 0 || row >= static_cast<int>(shown.lists.size())) {
        return;
    }
    work_.openEngineList(shown.engine, shown.lists[static_cast<std::size_t>(row)].id);
    emit opened();
}

} // namespace trackknife::bench
