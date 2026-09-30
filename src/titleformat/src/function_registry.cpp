// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/function_registry.hpp"

#include <array>

namespace trackknife::titleformat {
namespace {

constexpr auto unbounded = std::optional<std::size_t>{};

constexpr auto functions = std::to_array<FunctionSpec>({
    {FunctionId::if_, "if", 2, 3, EvaluationStrategy::lazy},
    {FunctionId::if2, "if2", 1, unbounded, EvaluationStrategy::lazy},
    {FunctionId::and_, "and", 0, unbounded, EvaluationStrategy::lazy},
    {FunctionId::or_, "or", 0, unbounded, EvaluationStrategy::lazy},
    {FunctionId::not_, "not", 1, 1, EvaluationStrategy::eager},
    {FunctionId::eq, "eq", 2, 2, EvaluationStrategy::eager},
    {FunctionId::ne, "ne", 2, 2, EvaluationStrategy::eager},
    {FunctionId::eqi, "eqi", 2, 2, EvaluationStrategy::eager},
    {FunctionId::gt, "gt", 2, 2, EvaluationStrategy::eager},
    {FunctionId::gte, "gte", 2, 2, EvaluationStrategy::eager},
    {FunctionId::lt, "lt", 2, 2, EvaluationStrategy::eager},
    {FunctionId::lte, "lte", 2, 2, EvaluationStrategy::eager},
    {FunctionId::add, "add", 0, unbounded, EvaluationStrategy::eager},
    {FunctionId::sub, "sub", 1, unbounded, EvaluationStrategy::eager},
    {FunctionId::mul, "mul", 0, unbounded, EvaluationStrategy::eager},
    {FunctionId::div, "div", 2, unbounded, EvaluationStrategy::eager},
    {FunctionId::mod, "mod", 2, 2, EvaluationStrategy::eager},
    {FunctionId::min, "min", 1, unbounded, EvaluationStrategy::eager},
    {FunctionId::max, "max", 1, unbounded, EvaluationStrategy::eager},
    {FunctionId::num, "num", 2, 2, EvaluationStrategy::eager},
    {FunctionId::decimal, "decimal", 3, 3, EvaluationStrategy::eager},
    {FunctionId::lower, "lower", 1, 1, EvaluationStrategy::eager},
    {FunctionId::upper, "upper", 1, 1, EvaluationStrategy::eager},
    {FunctionId::trim, "trim", 1, 1, EvaluationStrategy::eager},
    {FunctionId::len, "len", 1, 1, EvaluationStrategy::eager},
    {FunctionId::left, "left", 2, 2, EvaluationStrategy::eager},
    {FunctionId::right, "right", 2, 2, EvaluationStrategy::eager},
    {FunctionId::longest, "longest", 1, unbounded, EvaluationStrategy::eager},
    {FunctionId::repeat, "repeat", 2, 2, EvaluationStrategy::eager},
    {FunctionId::replace, "replace", 3, unbounded, EvaluationStrategy::eager},
    {FunctionId::pad, "pad", 2, 3, EvaluationStrategy::eager},
    {FunctionId::get, "get", 1, 1, EvaluationStrategy::eager},
    {FunctionId::getmulti, "getmulti", 2, 2, EvaluationStrategy::eager},
    {FunctionId::join, "join", 2, 2, EvaluationStrategy::eager},
    {FunctionId::lenmulti, "lenmulti", 1, 1, EvaluationStrategy::eager},
    {FunctionId::info, "info", 1, 1, EvaluationStrategy::eager},
    {FunctionId::each, "each", 1, 1, EvaluationStrategy::eager},
});

[[nodiscard]] constexpr bool equalsAsciiCaseInsensitive(const std::string_view left,
                                                        const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(left.at(index)) != lower(right.at(index))) {
            return false;
        }
    }
    return true;
}

} // namespace

const FunctionSpec* findFunction(const std::string_view name) noexcept {
    for (const auto& function : functions) {
        if (equalsAsciiCaseInsensitive(name, function.name)) {
            return &function;
        }
    }
    return nullptr;
}

} // namespace trackknife::titleformat
