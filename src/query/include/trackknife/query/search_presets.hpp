// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <span>
#include <string>
#include <string_view>

namespace trackknife::query {

enum class PresetInput { none, text, integer };

struct SearchPreset {
    std::string_view id;
    std::string_view topic;
    std::string_view title;
    std::string_view pattern;
    PresetInput input = PresetInput::none;
    std::string_view prompt = {};
    std::string_view example = {};
    int minimum = 0;
    int maximum = 0;
};

[[nodiscard]] std::span<const SearchPreset> search_presets();
// Pure tkq-1 generation: quotes user text, validates numeric bounds, and
// compiles the result before handing it to the ordinary search pipeline.
[[nodiscard]] core::Result<std::string> preset_query(const SearchPreset& preset,
                                                     std::string_view value = {});

} // namespace trackknife::query
