// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace trackknife::engine {

// Compares without an early exit, so how long a wrong guess takes to be
// refused says nothing about how much of it was right.
[[nodiscard]] bool same_token(std::string_view offered, std::string_view expected);

// A fresh random token, for a credential that lives as long as the engine.
[[nodiscard]] core::Result<std::string> random_token();

} // namespace trackknife::engine
