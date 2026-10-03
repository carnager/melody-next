// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_list_sync.hpp"

#include "bench/engine_playback.hpp"
#include "trackknife/protocol/message.hpp"

#include <QSettings>

#include <functional>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {
namespace {

using protocol::Json;
using persistence::EngineListItem;
using persistence::ListEdit;
using persistence::ListEntryPrint;

[[nodiscard]] Json optional_number(const std::optional<double>& value) {
    return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] std::optional<int> optional_int(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? std::optional{found->get<int>()}
                                                               : std::nullopt;
}

[[nodiscard]] std::vector<ListEntryPrint> prints_of(const std::vector<EngineListItem>& items) {
    std::vector<ListEntryPrint> prints;
    prints.reserve(items.size());
    for (const auto& item : items) {
        prints.push_back({.entry = item.entry_id, .print = EngineListSync::fingerprint(item)});
    }
    return prints;
}

[[nodiscard]] bool same_entries(const std::vector<ListEntryPrint>& one,
                                const std::vector<ListEntryPrint>& other) {
    return std::ranges::equal(one, other, [](const ListEntryPrint& a, const ListEntryPrint& b) {
        return a.entry == b.entry && a.print == b.print;
    });
}

// The items a list.get answer holds, as the engine stores them; an entry it
// could not describe is left out.
[[nodiscard]] std::vector<EngineListItem> items_of_answer(const Json& answer) {
    std::vector<EngineListItem> items;
    for (const auto& value : answer.value("items", Json::array())) {
        if (auto item = EngineListSync::itemFromJson(value)) {
            items.push_back(std::move(*item));
        }
    }
    return items;
}

[[nodiscard]] std::string id_text(const Json& answer) { return answer.value("id", std::string{}); }

// What an engine from before ADR-0256 takes in one line, less room for the
// rest of the request.
constexpr std::size_t old_engine_line_bytes = (1U << 20U) - 4096U;

} // namespace

Json EngineListSync::itemJson(const EngineListItem& item) {
    Json rendered{{"entry", item.entry_id.to_string()},
                  {"path", protocol::encode_raw_path(item.raw_path)},
                  {"title", protocol::displayable_text(item.title)},
                  {"artist", protocol::displayable_text(item.artist)},
                  {"album", protocol::displayable_text(item.album)}};
    if (!item.album_artist.empty()) {
        rendered["album_artist"] = protocol::displayable_text(item.album_artist);
    }
    if (!item.date.empty()) {
        rendered["date"] = protocol::displayable_text(item.date);
    }
    if (item.logical_reference) {
        rendered["logical"] = protocol::encode_raw_path(*item.logical_reference);
    }
    if (item.segment) {
        rendered["segment"] = Json{{"start_sample", item.segment->start_sample}};
        if (item.segment->end_sample) {
            rendered["segment"]["end_sample"] = *item.segment->end_sample;
        }
    }
    if (item.source_selection) {
        Json selection = Json::object();
        if (item.source_selection->audio_stream_index) {
            selection["stream_index"] = *item.source_selection->audio_stream_index;
        }
        if (item.source_selection->subsong_index) {
            selection["subsong_index"] = *item.source_selection->subsong_index;
        }
        rendered["selection"] = std::move(selection);
    }
    if (item.duration_ms && *item.duration_ms >= 0) {
        rendered["duration_ms"] = *item.duration_ms;
    }
    if (item.replay_gain) {
        rendered["replay_gain"] = Json{{"track_gain_db", optional_number(item.replay_gain->track_gain_db)},
                                       {"track_peak", optional_number(item.replay_gain->track_peak)},
                                       {"album_gain_db", optional_number(item.replay_gain->album_gain_db)},
                                       {"album_peak", optional_number(item.replay_gain->album_peak)}};
    }
    return rendered;
}

std::optional<EngineListItem> EngineListSync::itemFromJson(const Json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    auto path = protocol::decode_raw_path(value.value("path", std::string{}));
    auto entry = core::StableId::parse(value.value("entry", std::string{}));
    if (!path || !entry) {
        return std::nullopt;
    }
    EngineListItem item;
    item.entry_id = *entry;
    item.raw_path = std::move(*path);
    if (const auto logical = value.find("logical"); logical != value.end() && logical->is_string()) {
        if (auto decoded = protocol::decode_raw_path(logical->get<std::string>())) {
            item.logical_reference = std::move(*decoded);
        }
    }
    if (const auto segment = value.find("segment"); segment != value.end() && segment->is_object()) {
        const auto end = segment->find("end_sample");
        item.segment = persistence::ListItemSegment{
            .start_sample = segment->value("start_sample", std::int64_t{0}),
            .end_sample = end != segment->end() && end->is_number_integer()
                              ? std::optional{end->get<std::int64_t>()}
                              : std::nullopt};
    }
    if (const auto selection = value.find("selection");
        selection != value.end() && selection->is_object()) {
        persistence::ListItemSourceSelection chosen{
            .audio_stream_index = optional_int(*selection, "stream_index"),
            .subsong_index = optional_int(*selection, "subsong_index")};
        if (chosen.audio_stream_index || chosen.subsong_index) {
            item.source_selection = chosen;
        }
    }
    if (const auto duration = value.find("duration_ms");
        duration != value.end() && duration->is_number_integer() &&
        duration->get<std::int64_t>() >= 0) {
        item.duration_ms = duration->get<std::int64_t>();
    }
    const auto text = [&value](const char* key) {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>()
                                                          : std::string{};
    };
    item.title = text("title");
    item.artist = text("artist");
    item.album = text("album");
    item.album_artist = text("album_artist");
    item.date = text("date");
    if (const auto gain = value.find("replay_gain"); gain != value.end() && gain->is_object()) {
        const auto number = [&gain](const char* key) -> std::optional<double> {
            const auto found = gain->find(key);
            return found != gain->end() && found->is_number() ? std::optional{found->get<double>()}
                                                              : std::nullopt;
        };
        const persistence::ListItemReplayGain read{.track_gain_db = number("track_gain_db"),
                                                   .track_peak = number("track_peak"),
                                                   .album_gain_db = number("album_gain_db"),
                                                   .album_peak = number("album_peak")};
        if (read != persistence::ListItemReplayGain{}) {
            item.replay_gain = read;
        }
    }
    return item;
}

std::size_t EngineListSync::fingerprint(const EngineListItem& item) {
    // Every field the engine stores, each ended so neighbours cannot run
    // together. Text the wire cannot carry as it is reads back changed from
    // the engine, and is then sent once more as an update -- once.
    std::string text;
    text.reserve(item.raw_path.size() + item.title.size() + item.artist.size() +
                 item.album.size() + 96U);
    const auto add = [&text](const std::string_view part) {
        text += part;
        text += '\0';
    };
    const auto number = [&add](const auto& value) {
        add(value ? std::to_string(*value) : std::string{"-"});
    };
    add(item.raw_path);
    add(item.logical_reference.value_or(std::string{"\x01"}));
    number(item.segment ? std::optional{item.segment->start_sample} : std::nullopt);
    number(item.segment ? item.segment->end_sample : std::nullopt);
    number(item.source_selection ? item.source_selection->audio_stream_index : std::nullopt);
    number(item.source_selection ? item.source_selection->subsong_index : std::nullopt);
    number(item.duration_ms && *item.duration_ms >= 0 ? item.duration_ms : std::nullopt);
    add(item.title);
    add(item.artist);
    add(item.album);
    add(item.album_artist);
    add(item.date);
    const auto gain = item.replay_gain.value_or(persistence::ListItemReplayGain{});
    number(gain.track_gain_db);
    number(gain.track_peak);
    number(gain.album_gain_db);
    number(gain.album_peak);
    return std::hash<std::string>{}(text);
}

std::optional<persistence::ListDocument>
EngineListSync::documentFromAnswer(const Json& answer, const EngineKey& engine) {
    auto id = core::StableId::parse(id_text(answer));
    if (!id) {
        return std::nullopt;
    }
    persistence::ListDocument document{.id = *id,
                                       .kind = answer.value("kind", std::string{}) == "saved"
                                                   ? persistence::ListKind::saved
                                                   : persistence::ListKind::scratch,
                                       .name = answer.value("name", std::string{}),
                                       .pinned = false,
                                       .dirty = false,
                                       .items = {},
                                       .engine = engine.stored()};
    for (const auto& value : answer.value("items", Json::array())) {
        auto item = itemFromJson(value);
        if (!item) {
            continue;
        }
        persistence::ListItem row{.entry_id = item->entry_id,
                                  .source = persistence::ListSource::local,
                                  .profile_id = std::nullopt,
                                  .source_reference = item->raw_path,
                                  .logical_reference = std::move(item->logical_reference),
                                  .segment = item->segment,
                                  .source_selection = item->source_selection,
                                  .duration_ms = item->duration_ms,
                                  .source_revision = std::nullopt,
                                  .fields = {}};
        for (auto [name, field] :
             {std::pair{"title", &item->title}, std::pair{"artist", &item->artist},
              std::pair{"album", &item->album}, std::pair{"albumartist", &item->album_artist},
              std::pair{"date", &item->date}}) {
            if (!field->empty()) {
                row.fields.push_back({.name = name, .value = std::move(*field)});
            }
        }
        document.items.push_back(std::move(row));
    }
    return document;
}

EngineListSync::EngineListSync(QObject* parent) : QObject(parent) {
    for (const auto& pending : QSettings{}.value(QLatin1String(removals_key)).toStringList()) {
        // "engine<TAB>id"; before engines had keys, "r:id" or "l:id".
        if (const auto tab = pending.indexOf(QLatin1Char('\t')); tab > 0) {
            removals_.emplace(EngineKey::fromText(pending.left(tab)),
                              pending.mid(tab + 1).toStdString());
        } else if (pending.size() > 2 && pending.at(1) == QLatin1Char(':')) {
            removals_.emplace(pending.front() == QLatin1Char('r') ? EngineKey::remote()
                                                                  : EngineKey::local(),
                              pending.mid(2).toStdString());
        }
    }
}

void EngineListSync::opened(const Json& answer, const EngineKey& engine) {
    const auto id = id_text(answer);
    if (!core::StableId::parse(id)) {
        return;
    }
    auto& known = known_[id];
    known.engine = engine;
    known.working = answer.value("kind", std::string{}) != "saved";
    known.dirty = false;
    known.name = answer.value("name", std::string{});
    auto prints = prints_of(items_of_answer(answer));
    known.local = prints;
    known.acked = Acked{.revision = answer.value("revision", std::uint64_t{0}),
                        .name = known.name,
                        .working = known.working,
                        .entries = std::move(prints)};
    known.missing = false;
    // Open again: not to be deleted after all.
    if (removals_.erase({engine, id}) > 0) {
        storeRemovals();
    }
}

void EngineListSync::setEngine(const EngineKey& key, EnginePlayback* playback) {
    if (playback == nullptr) {
        engines_.erase(key);
        return;
    }
    auto& engine = engines_[key];
    if (engine.playback.data() != playback) {
        engine = Engine{};
        engine.playback = playback;
    }
    // Already connected, it will not say so again: what was left pending
    // last time goes now, not at some later reconnect.
    flushRemovals(key);
}

void EngineListSync::rekey(const EngineKey& from, const EngineKey& to) {
    if (from == to) {
        return;
    }
    if (auto moved = engines_.extract(from); !moved.empty()) {
        moved.key() = to;
        engines_.insert(std::move(moved));
    }
    for (auto& [id, known] : known_) {
        static_cast<void>(id);
        if (known.engine == from) {
            known.engine = to;
        }
    }
    const auto move_all = [&from, &to](std::set<std::pair<EngineKey, std::string>>& entries) {
        std::set<std::pair<EngineKey, std::string>> kept;
        for (const auto& [key, id] : entries) {
            kept.emplace(key == from ? to : key, id);
        }
        entries = std::move(kept);
    };
    move_all(removals_);
    move_all(removing_);
    storeRemovals();
}

EnginePlayback* EngineListSync::engineFor(const EngineKey& key) const {
    const auto found = engines_.find(key);
    if (found == engines_.end()) {
        return nullptr;
    }
    EnginePlayback* engine = found->second.playback.data();
    return engine != nullptr && engine->active() ? engine : nullptr;
}

EngineListSync::Engine& EngineListSync::stateFor(const EngineKey& key) { return engines_[key]; }

EngineKey EngineListSync::keyOf(const EnginePlayback* playback) const {
    for (const auto& [key, engine] : engines_) {
        if (playback != nullptr && engine.playback.data() == playback) {
            return key;
        }
    }
    return {};
}

void EngineListSync::take(const persistence::ListDocumentWrite& write, const ItemsOf& items_of) {
    const auto& document = write.document;
    auto& known = known_[document.id.to_string()];
    known.working = document.kind != persistence::ListKind::saved;
    known.engine = EngineKey::of(document);
    known.dirty = document.dirty;
    known.name = document.name;
    // Unchanged since the last save, a list keeps the entries read then; one
    // this window has not read yet is read once.
    if (write.items || !known.local) {
        if (auto items = items_of(document.id)) {
            known.local = prints_of(*items);
            known.items = std::make_shared<const std::vector<EngineListItem>>(std::move(*items));
        }
    }
}

bool EngineListSync::inStep(const Known& known) {
    return known.acked && known.local && !known.overwrite && known.acked->name == known.name &&
           known.acked->working == known.working && same_entries(known.acked->entries, *known.local);
}

void EngineListSync::settle(Known& known, const bool ready) {
    auto waiting = std::exchange(known.waiting, {});
    for (auto& then : waiting) {
        then(ready);
    }
}

void EngineListSync::update(const std::vector<persistence::ListDocumentWrite>& documents,
                            const ItemsOf& items_of) {
    items_of_ = items_of;
    std::unordered_set<std::string> present;
    for (const auto& write : documents) {
        // Server URIs of the retired MPD backend: no engine plays those.
        if (write.document.kind == persistence::ListKind::mpd) {
            continue;
        }
        present.insert(write.document.id.to_string());
        take(write, items_of);
    }
    for (const auto& id : present) {
        sync(id);
    }
    std::vector<EngineKey> keys;
    for (const auto& [key, engine] : engines_) {
        keys.push_back(key);
    }
    for (const auto& key : keys) {
        if (engineFor(key) != nullptr && !stateFor(key).compared) {
            compare(key);
        }
    }
    // Gone from the window: a working list goes from its engine too; a saved
    // one stays there, which is what saving it was for.
    for (auto entry = known_.begin(); entry != known_.end();) {
        if (present.contains(entry->first)) {
            ++entry;
            continue;
        }
        // Whether or not this window knows it was sent: after a reconnect or
        // a failed write it cannot tell, and the engine says if it had none.
        if (entry->second.working) {
            remove(entry->first, entry->second.engine);
        }
        settle(entry->second, false);
        entry = known_.erase(entry);
    }
}

void EngineListSync::sendNow(const persistence::ListDocumentWrite& document,
                             const ItemsOf& items_of, std::function<void(bool)> then) {
    items_of_ = items_of;
    const auto id = document.document.id.to_string();
    persistence::ListDocumentWrite current = document;
    current.items = true;
    take(current, items_of);
    auto& known = known_[id];
    known.waiting.push_back(std::move(then));
    // Not compared yet -- the window has only just connected: compared now,
    // and this goes as soon as that is done.
    if (engineFor(known.engine) != nullptr && !stateFor(known.engine).compared) {
        compare(known.engine);
    }
    sync(id);
}

void EngineListSync::sync(const std::string& id) {
    const auto found = known_.find(id);
    if (found == known_.end()) {
        return;
    }
    auto& known = found->second;
    if (engineFor(known.engine) == nullptr) {
        settle(known, false);
        return;
    }
    // Compared with the engine before anything is sent to it: this window's
    // copy may be older than what changed while it was closed. The
    // comparison's end asks for a save, which comes back here.
    if (!stateFor(known.engine).compared) {
        return;
    }
    // A saved list's unsaved edits stay in this window until Save.
    if (!known.working && known.dirty) {
        settle(known, false);
        return;
    }
    // Saved here after it changed there: the window settles which stands,
    // asked once.
    if (known.conflict) {
        if (!known.asked) {
            known.asked = true;
            emit conflicted(QString::fromStdString(id));
        }
        settle(known, false);
        return;
    }
    if (known.in_flight || known.fetching) {
        return;
    }
    if (inStep(known)) {
        known.items.reset();
        settle(known, true);
        return;
    }
    // Not known to the engine either way -- a failed write, an answer lost
    // with its connection: what it holds is read first.
    if (!known.acked && !known.missing) {
        fetch(id, known.engine, Fetch::rewrite);
        return;
    }
    if (!known.items || !known.local) {
        auto list_id = core::StableId::parse(id);
        auto items = list_id && items_of_ ? items_of_(*list_id) : std::nullopt;
        if (!items) {
            settle(known, false);
            return;
        }
        known.local = prints_of(*items);
        known.items = std::make_shared<const std::vector<EngineListItem>>(std::move(*items));
        if (inStep(known)) {
            known.items.reset();
            settle(known, true);
            return;
        }
    }
    // Held, not borrowed: a save while this is on its way replaces them.
    const auto items = known.items;
    if (stateFor(known.engine).whole_only) {
        sendWhole(id, known, *items);
    } else if (!known.acked) {
        create(id, known, *items);
    } else {
        edit(id, known, *items);
    }
}

void EngineListSync::create(const std::string& id, Known& known,
                            const std::vector<EngineListItem>& items) {
    // Made with as many items as one batch holds; the rest follow as inserts.
    auto batch = Json::array();
    std::vector<EngineListItem> sent;
    std::size_t bytes = 0;
    for (const auto& item : items) {
        auto rendered = itemJson(item);
        const auto size = rendered.dump().size() + 1U;
        if (!sent.empty() && bytes + size > batch_bytes) {
            break;
        }
        bytes += size;
        batch.push_back(std::move(rendered));
        sent.push_back(item);
    }
    Json params{{"id", id},
                {"name", protocol::displayable_text(known.name)},
                {"kind", known.working ? "working" : "saved"},
                {"items", std::move(batch)},
                {"revision", 0}};
    known.in_flight = true;
    ++in_flight_;
    const QPointer self{this};
    engineFor(known.engine)
        ->request(QStringLiteral("list.save"), std::move(params),
                  [self, id, sent = prints_of(sent), name = known.name,
                   working = known.working](core::Result<Json> answer) {
                      if (!self) {
                          return;
                      }
                      // As it was sent: a save while this was on its way
                      // changed the window's, not what the engine has.
                      self->written(id, answer,
                                    [&sent, &name, working](Known& list, const Json& summary) {
                                        list.acked = Acked{
                                            .revision = summary.value("revision", std::uint64_t{0}),
                                            .name = name,
                                            .working = working,
                                            .entries = sent};
                                        list.missing = false;
                                    });
                  },
                  true);
}

void EngineListSync::edit(const std::string& id, Known& known,
                          const std::vector<EngineListItem>& items) {
    // What the engine holds leading what is shown -- a long list being made,
    // batch after batch -- needs no working out: the rest goes after it.
    const auto& held = known.acked->entries;
    const auto& wanted = *known.local;
    std::vector<persistence::PlannedListEdit> plan;
    if (held.size() < wanted.size() &&
        std::equal(held.begin(), held.end(), wanted.begin(),
                   [](const ListEntryPrint& a, const ListEntryPrint& b) {
                       return a.entry == b.entry && a.print == b.print;
                   })) {
        persistence::PlannedListEdit rest{.kind = ListEdit::Kind::insert,
                                          .entries = {},
                                          .rows = {},
                                          .after = held.empty() ? std::nullopt
                                                                : std::optional{held.back().entry}};
        rest.rows.reserve(wanted.size() - held.size());
        for (auto row = held.size(); row < wanted.size(); ++row) {
            rest.rows.push_back(row);
        }
        plan.push_back(std::move(rest));
    } else {
        plan = persistence::plan_list_edits(held, wanted);
    }
    // As much of the plan as one batch holds, in order. What is left is
    // worked out again once this is answered, from what the engine then has.
    auto edits = Json::array();
    std::vector<ListEdit> sent;
    std::size_t bytes = 0;
    bool full = false;
    for (const auto& planned : plan) {
        ListEdit piece{.kind = planned.kind, .entries = {}, .items = {}, .after = planned.after};
        auto listed = Json::array();
        const auto fits = [&](const std::size_t size) {
            if ((!sent.empty() || !listed.empty()) && bytes + size > batch_bytes) {
                full = true;
                return false;
            }
            bytes += size;
            return true;
        };
        for (const auto& entry : planned.entries) {
            auto text = entry.to_string();
            if (!fits(text.size() + 3U)) {
                break;
            }
            listed.push_back(std::move(text));
            piece.entries.push_back(entry);
        }
        for (const auto row : planned.rows) {
            auto rendered = itemJson(items[row]);
            if (!fits(rendered.dump().size() + 1U)) {
                break;
            }
            listed.push_back(std::move(rendered));
            piece.items.push_back(items[row]);
        }
        if (!listed.empty()) {
            Json op;
            switch (planned.kind) {
            case ListEdit::Kind::remove:
                op = Json{{"remove", std::move(listed)}};
                break;
            case ListEdit::Kind::insert:
                op = Json{{"insert", std::move(listed)}};
                break;
            case ListEdit::Kind::move:
                op = Json{{"move", std::move(listed)}};
                break;
            case ListEdit::Kind::update:
                op = Json{{"update", std::move(listed)}};
                break;
            }
            if (planned.kind == ListEdit::Kind::insert || planned.kind == ListEdit::Kind::move) {
                op["after"] = planned.after ? Json(planned.after->to_string()) : Json(nullptr);
            }
            edits.push_back(std::move(op));
            sent.push_back(std::move(piece));
        }
        if (full) {
            break;
        }
    }
    Json params{{"id", id}, {"revision", known.acked->revision}, {"edits", std::move(edits)}};
    if (known.acked->name != known.name) {
        params["name"] = protocol::displayable_text(known.name);
    }
    if (known.acked->working != known.working) {
        params["kind"] = known.working ? "working" : "saved";
    }
    known.in_flight = true;
    ++in_flight_;
    const QPointer self{this};
    engineFor(known.engine)
        ->request(QStringLiteral("list.edit"), std::move(params),
                  [self, id, sent = std::move(sent), name = known.name,
                   working = known.working](core::Result<Json> answer) {
                      if (!self) {
                          return;
                      }
                      self->written(id, answer, [&sent, &name, working](Known& list,
                                                                        const Json& summary) {
                          if (!list.acked) {
                              return;
                          }
                          auto applied = persistence::apply_list_edits(
                              list.acked->entries, sent,
                              [](const ListEntryPrint& entry) { return entry.entry; },
                              [](const EngineListItem& item) {
                                  return ListEntryPrint{.entry = item.entry_id,
                                                        .print = fingerprint(item)};
                              });
                          if (!applied) {
                              // The engine took what this window cannot
                              // follow: what it holds is read again.
                              list.acked.reset();
                              return;
                          }
                          // The name and kind as sent with it: a save on the
                          // way since is the next edit's.
                          list.acked->revision = summary.value("revision", std::uint64_t{0});
                          list.acked->name = name;
                          list.acked->working = working;
                      });
                  },
                  true);
}

void EngineListSync::sendWhole(const std::string& id, Known& known,
                               const std::vector<EngineListItem>& items) {
    // An engine from before ADR-0256: the whole list, as then. What does not
    // fit in its line is refused, and said so, rather than looping.
    auto rendered = Json::array();
    std::size_t bytes = 0;
    for (const auto& item : items) {
        rendered.push_back(itemJson(item));
        bytes += rendered.back().dump().size() + 1U;
    }
    // Such an engine drops a line past its limit without a word, and the
    // window would reconnect and send it again for good: a list that long
    // stays in the window until the engine is updated.
    if (bytes > old_engine_line_bytes) {
        qWarning("A list of %zu entries is too long for an engine from before ADR-0256; "
                 "it is kept in this window until that engine is updated",
                 items.size());
        settle(known, false);
        return;
    }
    Json params{{"id", id},
                {"name", protocol::displayable_text(known.name)},
                {"kind", known.working ? "working" : "saved"},
                {"items", std::move(rendered)}};
    if (!known.working && known.acked && !known.overwrite) {
        params["revision"] = known.acked->revision;
    }
    known.in_flight = true;
    ++in_flight_;
    const QPointer self{this};
    engineFor(known.engine)
        ->request(QStringLiteral("list.save"), std::move(params),
                  [self, id, sent = prints_of(items), name = known.name,
                   working = known.working](core::Result<Json> answer) {
                      if (!self) {
                          return;
                      }
                      self->written(id, answer,
                                    [&sent, &name, working](Known& list, const Json& summary) {
                                        list.acked = Acked{
                                            .revision = summary.value("revision", std::uint64_t{0}),
                                            .name = name,
                                            .working = working,
                                            .entries = sent};
                                        list.missing = false;
                                    });
                  },
                  true);
}

void EngineListSync::written(const std::string& id, const core::Result<Json>& answer,
                             const std::function<void(Known&, const Json&)>& applied) {
    --in_flight_;
    const auto found = known_.find(id);
    if (found == known_.end()) {
        return;
    }
    auto& known = found->second;
    known.in_flight = false;
    if (answer) {
        applied(known, *answer);
        known.overwrite = false;
        // Whatever is left -- the rest of a long list, or what changed while
        // this was on its way -- goes next.
        sync(id);
        return;
    }
    switch (answer.error().code) {
    case core::ErrorCode::conflict:
        // Moved on since: a working list is this window's scratch, and its
        // version stands, worked out again against what the engine has; a
        // saved one is the window's to settle.
        if (known.working || known.overwrite) {
            known.acked.reset();
            known.missing = false;
            sync(id);
            return;
        }
        known.conflict = true;
        known.asked = true;
        emit conflicted(QString::fromStdString(id));
        settle(known, false);
        return;
    case core::ErrorCode::not_found:
        // The list, or an entry the edit named, is not what this window
        // thought: what the engine holds is read again.
        known.acked.reset();
        known.missing = false;
        sync(id);
        return;
    case core::ErrorCode::unsupported:
        // An engine from before list.edit.
        if (!stateFor(known.engine).whole_only) {
            stateFor(known.engine).whole_only = true;
            sync(id);
            return;
        }
        break;
    default:
        break;
    }
    // Tried again at the next save. Whether the engine took it is not known
    // -- an answer lost with its connection -- so a write against the old
    // revision then finds out.
    settle(known, false);
}

void EngineListSync::reconnected(const EnginePlayback* engine) {
    const auto key = keyOf(engine);
    if (key.isNull()) {
        return;
    }
    auto& state = stateFor(key);
    const QPointer<EnginePlayback> playback = state.playback;
    state = Engine{};
    state.playback = playback;
    flushRemovals(key);
    // What each list was acknowledged at is kept: comparing revisions says
    // whether anything changed while away, and nothing is sent for it.
    emit wantsSave();
}

void EngineListSync::compare(const EngineKey& key) {
    auto& state = stateFor(key);
    auto* engine = engineFor(key);
    if (state.comparing || engine == nullptr) {
        return;
    }
    state.comparing = true;
    ++in_flight_;
    const QPointer self{this};
    engine->request(QStringLiteral("list.all"), Json::object(),
                    [self, key](core::Result<Json> answer) {
                        if (!self) {
                            return;
                        }
                        --self->in_flight_;
                        std::unordered_map<std::string, std::uint64_t> listed;
                        if (answer) {
                            for (const auto& list : answer->value("lists", Json::array())) {
                                listed.emplace(id_text(list),
                                               list.value("revision", std::uint64_t{0}));
                            }
                        }
                        // At the revision acknowledged: in step, nothing to
                        // read. Another: read and compared. Not there: made.
                        std::vector<std::string> wanted;
                        for (auto& [id, known] : self->known_) {
                            if (known.engine != key) {
                                continue;
                            }
                            const auto there = listed.find(id);
                            if (there == listed.end()) {
                                if (answer) {
                                    known.acked.reset();
                                    known.missing = true;
                                }
                                continue;
                            }
                            if (!known.acked || known.acked->revision != there->second) {
                                wanted.push_back(id);
                            }
                        }
                        for (const auto& id : wanted) {
                            self->fetch(id, key, Fetch::compare);
                        }
                        self->compared(key);
                    });
}

void EngineListSync::compared(const EngineKey& key) {
    auto& state = stateFor(key);
    if (state.outstanding > 0 || state.compared) {
        return;
    }
    state.comparing = false;
    state.compared = true;
    // What waits to play goes now, not at the next save.
    std::vector<std::string> waiting;
    for (const auto& [id, known] : known_) {
        if (known.engine == key && !known.waiting.empty()) {
            waiting.push_back(id);
        }
    }
    for (const auto& id : waiting) {
        sync(id);
    }
    emit wantsSave();
}

void EngineListSync::fetch(const std::string& id, const EngineKey& key, const Fetch why) {
    auto* engine = engineFor(key);
    const auto found = known_.find(id);
    if (engine == nullptr || found == known_.end()) {
        return;
    }
    found->second.fetching = true;
    ++in_flight_;
    if (why == Fetch::compare) {
        ++stateFor(key).outstanding;
    }
    const QPointer self{this};
    engine->request(QStringLiteral("list.get"), Json{{"id", id}},
                    [self, id, key, why](core::Result<Json> answer) {
                        if (!self) {
                            return;
                        }
                        --self->in_flight_;
                        const auto finish = [&self, &key, why] {
                            if (why == Fetch::compare) {
                                --self->stateFor(key).outstanding;
                                self->compared(key);
                            }
                        };
                        const auto held = self->known_.find(id);
                        if (held == self->known_.end()) {
                            finish();
                            return;
                        }
                        auto& known = held->second;
                        known.fetching = false;
                        if (!answer) {
                            if (answer.error().code == core::ErrorCode::not_found) {
                                // Not there: made again, whole.
                                known.acked.reset();
                                known.missing = true;
                                if (why != Fetch::compare) {
                                    self->sync(id);
                                }
                            } else {
                                self->settle(known, false);
                            }
                            finish();
                            return;
                        }
                        auto entries = prints_of(items_of_answer(*answer));
                        const bool same = known.local && same_entries(*known.local, entries);
                        known.acked = Acked{.revision = answer->value("revision", std::uint64_t{0}),
                                            .name = answer->value("name", std::string{}),
                                            .working = answer->value("kind", std::string{}) != "saved",
                                            .entries = std::move(entries)};
                        known.missing = false;
                        if (why == Fetch::rewrite || same) {
                            // This window's version goes on from what the
                            // engine has.
                            if (why == Fetch::rewrite) {
                                self->sync(id);
                            }
                        } else if (!known.working && known.dirty) {
                            // Changed there while edited here: settled when saved.
                            known.conflict = true;
                        } else if (auto document = documentFromAnswer(*answer, key)) {
                            known.local = known.acked->entries;
                            known.name = known.acked->name;
                            known.working = known.acked->working;
                            known.conflict = false;
                            known.asked = false;
                            emit self->adopted(*document);
                        }
                        finish();
                    });
}

void EngineListSync::listChanged(const EnginePlayback* engine, const QString& id,
                                 const quint64 revision, const bool deleted) {
    const auto key = keyOf(engine);
    const auto found = known_.find(id.toStdString());
    if (key.isNull() || found == known_.end() || found->second.engine != key) {
        return;
    }
    auto& known = found->second;
    // Deleted elsewhere: that stands, even over a write of this window's on
    // its way -- which, landing after the delete, would make the list again.
    // So it is deleted again, after that write on the same connection.
    if (deleted) {
        const bool writing = known.in_flight;
        settle(known, false);
        known_.erase(found);
        if (writing) {
            remove(id.toStdString(), key);
        }
        emit removedElsewhere(id);
        return;
    }
    // This window's own write: its answer brings the revision.
    if (known.in_flight || known.fetching) {
        return;
    }
    if (known.acked && revision <= known.acked->revision) {
        return;
    }
    fetch(id.toStdString(), key, Fetch::elsewhere);
}

void EngineListSync::keepMine(const QString& id) {
    const auto found = known_.find(id.toStdString());
    if (found == known_.end()) {
        return;
    }
    found->second.conflict = false;
    found->second.asked = false;
    // Written over whatever changed there: worked out against what the
    // engine has now.
    found->second.overwrite = true;
    found->second.acked.reset();
    found->second.missing = false;
    emit wantsSave();
}

void EngineListSync::takeTheirs(const QString& id) {
    const auto found = known_.find(id.toStdString());
    if (found == known_.end()) {
        return;
    }
    found->second.conflict = false;
    found->second.asked = false;
    found->second.dirty = false;
    found->second.local.reset();
    fetch(id.toStdString(), found->second.engine, Fetch::elsewhere);
}

void EngineListSync::remove(const std::string& id, const EngineKey& key) {
    // Kept until the engine has answered, across restarts: one that is away
    // is told when it is back, or the list stays on it for good.
    removals_.emplace(key, id);
    storeRemovals();
    flushRemovals(key);
}

void EngineListSync::flushRemovals(const EngineKey& key) {
    auto* engine = engineFor(key);
    if (engine == nullptr || !engine->active()) {
        return;
    }
    for (const auto& pending : removals_) {
        if (pending.first != key || removing_.contains(pending)) {
            continue;
        }
        removing_.insert(pending);
        ++in_flight_;
        const QPointer self{this};
        engine->request(QStringLiteral("list.delete"), Json{{"id", pending.second}},
                        [self, pending](const core::Result<Json>& answer) {
                            if (!self) {
                                return;
                            }
                            --self->in_flight_;
                            self->removing_.erase(pending);
                            // Gone, or an engine too old to have lists: done.
                            // Anything else is tried again on reconnecting.
                            if (answer || answer.error().code == core::ErrorCode::not_found ||
                                answer.error().code == core::ErrorCode::unsupported) {
                                self->removals_.erase(pending);
                                self->storeRemovals();
                            }
                        });
    }
}

void EngineListSync::storeRemovals() const {
    QStringList stored;
    for (const auto& [key, id] : removals_) {
        stored.push_back(key.text() + QLatin1Char('\t') + QString::fromStdString(id));
    }
    QSettings settings;
    if (stored.isEmpty()) {
        settings.remove(QLatin1String(removals_key));
    } else {
        settings.setValue(QLatin1String(removals_key), stored);
    }
}

} // namespace trackknife::bench
