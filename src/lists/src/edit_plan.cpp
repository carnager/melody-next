// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/lists/edit_plan.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <tuple>

namespace trackknife::lists {
namespace {
core::Error error(std::string message, core::ErrorCode code = core::ErrorCode::invalid_argument) {
    return {.code = code, .message = std::move(message), .context = {}};
}
class Context final : public titleformat::EvaluationContext {
  public:
    explicit Context(const Entry& entry) : entry_(entry) {}
    titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::sort;
    }
    std::optional<MetadataValues> resolveMetadata(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_field_name(name);
        auto values = entry_.metadata.effective_values(canonical);
        if (values.empty()) {
            constexpr std::array names{"title",       "artist", "album",
                                       "albumartist", "date",   "tracknumber"};
            for (std::size_t field = 0; field < names.size(); ++field)
                if (canonical == names[field] && !entry_.display[field].empty())
                    values.push_back(entry_.display[field]);
            if (values.empty() && canonical == "title")
                values.push_back(
                    core::escape_raw_path(std::filesystem::path{entry_.raw_path}.stem().native()));
        }
        return values.empty() ? std::nullopt : std::optional{std::move(values)};
    }
    std::optional<std::string> resolveField(std::string_view name) const override {
        const auto values = resolveMetadata(name);
        if (!values)
            return std::nullopt;
        std::string text;
        for (std::size_t index = 0; index < values->size(); ++index) {
            if (index != 0)
                text += "; ";
            text += values->at(index);
        }
        return text;
    }
    std::optional<std::string> resolveTechnicalInfo(std::string_view name) const override {
        const auto canonical = metadata::canonicalize_native_field_name(name);
        const std::filesystem::path path{entry_.raw_path};
        if (canonical == "path")
            return core::escape_raw_path(entry_.raw_path);
        if (canonical == "filename")
            return core::escape_raw_path(path.stem().native());
        if (canonical == "filename_ext")
            return core::escape_raw_path(path.filename().native());
        if (canonical == "directory")
            return core::escape_raw_path(path.parent_path().native());
        if (canonical == "extension") {
            auto extension = path.extension().native();
            if (extension.starts_with('.'))
                extension.erase(extension.begin());
            return core::escape_raw_path(extension);
        }
        return std::nullopt;
    }

  private:
    const Entry& entry_;
};
// Compare ASCII digit runs numerically without integer overflow. Equal numeric
// runs (including leading zeros) preserve stability; other text is byte ordered
// after repository-owned Unicode simple lowercase mapping.
bool natural_less(std::string_view left, std::string_view right) {
    const auto digit = [](char value) { return value >= '0' && value <= '9'; };
    std::size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (digit(left[a]) && digit(right[b])) {
            auto a_end = a, b_end = b;
            while (a_end < left.size() && digit(left[a_end]))
                ++a_end;
            while (b_end < right.size() && digit(right[b_end]))
                ++b_end;
            while (a < a_end && left[a] == '0')
                ++a;
            while (b < b_end && right[b] == '0')
                ++b;
            if (a_end - a != b_end - b)
                return a_end - a < b_end - b;
            const auto comparison = left.substr(a, a_end - a).compare(right.substr(b, b_end - b));
            if (comparison != 0)
                return comparison < 0;
            a = a_end;
            b = b_end;
        } else {
            if (left[a] != right[b])
                return static_cast<unsigned char>(left[a]) < static_cast<unsigned char>(right[b]);
            ++a;
            ++b;
        }
    }
    return a == left.size() && b != right.size();
}

auto identity(const Entry& entry) {
    return std::tuple{std::string_view{entry.raw_path},
                      entry.logical_reference
                          ? std::optional{std::string_view{*entry.logical_reference}}
                          : std::nullopt,
                      entry.selection.stream_index,
                      entry.selection.subsong_index,
                      entry.segment.has_value(),
                      entry.segment ? entry.segment->start_sample : 0,
                      entry.segment ? entry.segment->end_sample : std::nullopt};
}
} // namespace

core::Result<EditPlan> plan_reverse(std::size_t rows, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(error("List edit cancelled", core::ErrorCode::cancelled));
    if (rows > 1'000'000U)
        return std::unexpected(
            error("List edits support at most one million rows", core::ErrorCode::limit_exceeded));
    EditPlan plan;
    plan.positions.reserve(rows);
    for (std::size_t row = rows; row > 0; --row) {
        if (cancellation.is_cancellation_requested())
            return std::unexpected(error("List edit cancelled", core::ErrorCode::cancelled));
        plan.positions.push_back(static_cast<int>(row - 1));
    }
    return plan;
}

core::Result<EditPlan> plan_edit(std::span<const Entry> entries, const EditRequest& request,
                                 const core::CancellationToken& cancellation,
                                 std::atomic<std::size_t>* progress) {
    try {
        const auto check = [&] {
            if (cancellation.is_cancellation_requested())
                throw error("List edit cancelled", core::ErrorCode::cancelled);
        };
        check();
        if (entries.size() > 1'000'000U)
            return std::unexpected(error("List edits support at most one million rows",
                                         core::ErrorCode::limit_exceeded));
        EditPlan plan;
        if (request.kind == EditKind::shuffle_albums) {
            std::map<std::array<std::string, 3>, std::size_t> albums;
            std::vector<std::vector<int>> groups;
            std::size_t key_bytes = 0;
            for (std::size_t row = 0; row < entries.size(); ++row) {
                check();
                const auto& fields = entries[row].display;
                const auto& album = fields[2];
                const auto& artist = fields[3].empty() ? fields[1] : fields[3];
                const auto& date = fields[4];
                const auto bytes = artist.size() + album.size() + date.size();
                key_bytes += bytes;
                if (bytes > 64U * 1024U || key_bytes > 64U * 1024U * 1024U)
                    return std::unexpected(
                        error("Album grouping keys exceed the list-edit memory limit",
                              core::ErrorCode::limit_exceeded));
                std::size_t group = groups.size();
                if (!album.empty()) {
                    const auto [it, inserted] =
                        albums.try_emplace(std::array{artist, album, date}, group);
                    group = it->second;
                    if (inserted)
                        groups.emplace_back();
                } else
                    groups.emplace_back();
                groups[group].push_back(static_cast<int>(row));
                if (progress)
                    progress->store(row + 1);
            }
            std::mt19937 generator{request.seed};
            // Explicit Fisher-Yates keeps cancellation responsive for large lists.
            for (std::size_t size = groups.size(); size > 1; --size) {
                check();
                const auto other =
                    std::uniform_int_distribution<std::size_t>{0, size - 1}(generator);
                std::swap(groups[size - 1], groups[other]);
            }
            plan.positions.reserve(entries.size());
            for (const auto& group : groups) {
                check();
                plan.positions.insert(plan.positions.end(), group.begin(), group.end());
            }
            return plan;
        }
        if (request.kind == EditKind::remove_duplicates) {
            plan.removal = true;
            const auto less = [&](int left, int right) {
                check();
                return identity(entries[static_cast<std::size_t>(left)]) <
                       identity(entries[static_cast<std::size_t>(right)]);
            };
            std::set<int, decltype(less)> seen{less};
            for (std::size_t row = 0; row < entries.size(); ++row) {
                check();
                if (!seen.insert(static_cast<int>(row)).second)
                    plan.positions.push_back(static_cast<int>(row));
                if (progress)
                    progress->store(row + 1);
            }
            return plan;
        }
        if (request.kind == EditKind::reverse)
            return plan_reverse(entries.size(), cancellation);
        plan.positions.resize(entries.size());
        std::iota(plan.positions.begin(), plan.positions.end(), 0);
        if (request.expression.empty() || request.expression.size() > 4096U)
            return std::unexpected(error("Enter a sorting expression of at most 4096 bytes"));
        const auto compiled = titleformat::compile(
            request.expression,
            {.context = titleformat::FormatContextKind::sort, .dialect = {}, .parse_options = {}});
        if (!compiled.isValid()) {
            const auto message = !compiled.diagnostics.empty()
                                     ? compiled.diagnostics.front().message
                                     : "Invalid tkfmt-1 sorting expression";
            return std::unexpected(error(message));
        }
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        std::size_t bytes = 0;
        for (const auto& entry : entries) {
            check();
            const Context context{entry};
            const auto result = titleformat::evaluate(*compiled.program, context,
                                                      {.maximum_steps = 100'000U,
                                                       .maximum_output_bytes = 64U * 1024U,
                                                       .maximum_expanded_results = 1U,
                                                       .cancellation = cancellation});
            if (!result)
                return std::unexpected(result.error());
            auto key = core::unicodeSimpleLower(result->text);
            if (!key)
                return std::unexpected(key.error());
            if (key->size() > 64U * 1024U)
                return std::unexpected(
                    error("A sorting key exceeds 64 KiB", core::ErrorCode::limit_exceeded));
            bytes += key->size();
            if (bytes > 64U * 1024U * 1024U)
                return std::unexpected(
                    error("Sorting keys exceed 64 MiB", core::ErrorCode::limit_exceeded));
            keys.push_back(std::move(*key));
            if (progress)
                progress->store(keys.size());
        }
        std::stable_sort(plan.positions.begin(), plan.positions.end(), [&](int left, int right) {
            check();
            const auto& a = keys[static_cast<std::size_t>(left)];
            const auto& b = keys[static_cast<std::size_t>(right)];
            return request.descending ? natural_less(b, a) : natural_less(a, b);
        });
        check();
        return plan;
    } catch (const core::Error& problem) {
        return std::unexpected(problem);
    }
}
} // namespace trackknife::lists
