// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/titleformat/function_registry.hpp"
#include "trackknife/titleformat/parser.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::titleformat {

enum class FormatContextKind {
    track_display,
    now_playing,
    list_item,
    queue,
    path_generation,
    tree_level,
    grouping,
    copy_export,
    sort,
    metadata_transformation,
};

struct DialectVersion {
    std::string dialect{"tkfmt"};
    std::uint32_t dialect_version{1};
    std::uint32_t compiler_schema{2};

    friend bool operator==(const DialectVersion&, const DialectVersion&) = default;
};

struct ProgramCacheKey {
    DialectVersion dialect;
    FormatContextKind context{FormatContextKind::track_display};
    std::string source;
    // 2: $decimal. A program compiled against an older registry -- where it
    // was an unknown function -- is not reused.
    std::uint32_t function_registry_revision{2};

    friend bool operator==(const ProgramCacheKey&, const ProgramCacheKey&) = default;
};

struct CompileOptions {
    FormatContextKind context{FormatContextKind::track_display};
    DialectVersion dialect;
    ParseOptions parse_options;
};

enum class CompileDiagnosticCode {
    unsupported_dialect,
    unknown_function,
    wrong_argument_count,
    unavailable_in_context,
    invalid_argument
};

struct CompileDiagnostic {
    CompileDiagnosticCode code;
    SourceSpan span;
    std::string message;

    friend bool operator==(const CompileDiagnostic&, const CompileDiagnostic&) = default;
};

class Program final {
  public:
    [[nodiscard]] const SyntaxTree& syntax() const noexcept { return syntax_; }
    [[nodiscard]] FormatContextKind context() const noexcept { return context_; }
    [[nodiscard]] const DialectVersion& dialectVersion() const noexcept { return dialect_; }
    [[nodiscard]] const std::vector<std::string>& fieldDependencies() const noexcept {
        return field_dependencies_;
    }
    [[nodiscard]] const std::vector<std::string>& expansionDependencies() const noexcept {
        return expansion_dependencies_;
    }
    [[nodiscard]] const std::vector<std::string>& technicalDependencies() const noexcept {
        return technical_dependencies_;
    }
    [[nodiscard]] bool hasExpansions() const noexcept { return !expansion_dependencies_.empty(); }
    [[nodiscard]] ProgramCacheKey cacheKey() const {
        return {dialect_, context_, syntax_.source(), 1U};
    }
    [[nodiscard]] std::optional<FunctionId> functionAt(NodeId node) const;

  private:
    SyntaxTree syntax_;
    FormatContextKind context_{FormatContextKind::track_display};
    DialectVersion dialect_;
    std::vector<std::optional<FunctionId>> resolved_functions_;
    std::vector<std::string> field_dependencies_;
    std::vector<std::string> expansion_dependencies_;
    std::vector<std::string> technical_dependencies_;

    friend class Compiler;
};

struct CompileOutput {
    std::optional<Program> program;
    std::vector<Diagnostic> parse_diagnostics;
    std::vector<CompileDiagnostic> diagnostics;

    [[nodiscard]] bool isValid() const noexcept {
        return program.has_value() && parse_diagnostics.empty() && diagnostics.empty();
    }
};

[[nodiscard]] CompileOutput compile(std::string source, CompileOptions options = {});

} // namespace trackknife::titleformat
