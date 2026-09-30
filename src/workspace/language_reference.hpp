// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

namespace trackknife::bench {

// The language documents Trackknife ships inside itself: tkfmt-1, and the
// tagging script written as text (ADR-0241).
enum class LanguageReference { formatting, scripts };

// Both written as web pages beside each other in `directory` -- each
// linking to the other -- and the one asked for's path returned; empty, with
// `error`, when they could not be.
[[nodiscard]] QString writeLanguageReference(LanguageReference reference, const QString& directory,
                                             QString* error = nullptr);

// Written into the cache and opened in the browser. False, with `error`,
// when the page could not be written or no browser took it.
[[nodiscard]] bool openLanguageReference(LanguageReference reference, QString* error = nullptr);

} // namespace trackknife::bench
