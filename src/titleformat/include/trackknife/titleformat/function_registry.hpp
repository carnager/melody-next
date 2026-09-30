// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace trackknife::titleformat {

enum class FunctionId {
    if_,
    if2,
    and_,
    or_,
    not_,
    eq,
    ne,
    eqi,
    gt,
    gte,
    lt,
    lte,
    add,
    sub,
    mul,
    div,
    mod,
    min,
    max,
    num,
    decimal,
    lower,
    upper,
    trim,
    len,
    left,
    right,
    longest,
    repeat,
    replace,
    pad,
    get,
    getmulti,
    join,
    lenmulti,
    info,
    each,
};

enum class EvaluationStrategy { eager, lazy };

struct FunctionSpec {
    FunctionId id;
    std::string_view name;
    std::size_t minimum_arguments;
    std::optional<std::size_t> maximum_arguments;
    EvaluationStrategy strategy;

    [[nodiscard]] bool acceptsArity(std::size_t count) const noexcept {
        return count >= minimum_arguments && (!maximum_arguments || count <= *maximum_arguments);
    }
};

[[nodiscard]] const FunctionSpec* findFunction(std::string_view name) noexcept;

} // namespace trackknife::titleformat
