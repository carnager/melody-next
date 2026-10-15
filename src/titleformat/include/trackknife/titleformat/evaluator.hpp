// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/value.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::titleformat {

class EvaluationContext {
  public:
    using MetadataValues = std::vector<std::string>;

    virtual ~EvaluationContext() = default;

    [[nodiscard]] virtual FormatContextKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::string> resolveField(std::string_view name) const = 0;

    // Missing means the raw tag is absent. The vector preserves source order; a present
    // empty value is represented by a vector containing an empty string.
    [[nodiscard]] virtual std::optional<MetadataValues>
    resolveMetadata(std::string_view name) const {
        auto value = resolveField(name);
        if (!value) {
            return std::nullopt;
        }
        MetadataValues values;
        values.push_back(std::move(*value));
        return values;
    }

    // Technical information is a separate namespace from raw metadata.
    [[nodiscard]] virtual std::optional<std::string> resolveTechnicalInfo(std::string_view) const {
        return std::nullopt;
    }
};

struct EvaluationOptions {
    std::size_t maximum_steps{100'000U};
    std::size_t maximum_output_bytes{1024U * 1024U};
    std::size_t maximum_expanded_results{256U};
    core::CancellationToken cancellation;
    // For text shown on screen only, never a file name or a key: a value set
    // into the expression's own text -- a field, a function's result -- that
    // holds right-to-left letters is wrapped in Unicode first-strong isolates
    // (U+2068 ... U+2069), so a Hebrew or Arabic title does not reorder the
    // text after it. Text with none is unchanged.
    bool isolate_values{false};
};

[[nodiscard]] core::Result<EvalValue>
evaluate(const Program& program, const EvaluationContext& context, EvaluationOptions options = {});

[[nodiscard]] core::Result<std::vector<EvalValue>>
evaluateExpanded(const Program& program, const EvaluationContext& context,
                 EvaluationOptions options = {});

using EvaluationContextRef = std::reference_wrapper<const EvaluationContext>;

[[nodiscard]] std::vector<core::Result<EvalValue>>
evaluateBatch(const Program& program, std::span<const EvaluationContextRef> contexts,
              EvaluationOptions options = {});

} // namespace trackknife::titleformat
