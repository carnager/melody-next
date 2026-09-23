// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace trackknife::engine {

// ADR-0223: the credential a TCP client presents.
//
// Created on first use with mode 0600 and reused after that, so a client
// configured once keeps working across restarts. Rotating it is deleting the
// file. The file is created exclusively, so two engines starting at once
// cannot each write a different token and leave one of them lying.
//
// An existing file that group or others can read is refused rather than used.
// Anyone who can read the token controls the engine, and silently accepting a
// loosened file is how a credential ends up world-readable without anyone
// deciding it should be -- the same rule ssh applies to private keys.
[[nodiscard]] core::Result<std::string> load_or_create_token(const std::filesystem::path& path);

// Compares without an early exit, so how long a wrong guess takes to be
// refused says nothing about how much of it was right.
[[nodiscard]] bool same_token(std::string_view offered, std::string_view expected);

// A fresh random token, for a credential that lives as long as the engine.
[[nodiscard]] core::Result<std::string> random_token();

} // namespace trackknife::engine
