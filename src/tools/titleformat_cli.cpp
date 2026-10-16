// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using trackknife::titleformat::FormatContextKind;

[[nodiscard]] std::string asciiLower(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
                             ? static_cast<char>(character - 'A' + 'a')
                             : character);
    }
    return result;
}

[[nodiscard]] std::optional<FormatContextKind> parseContext(const std::string_view value) {
    if (value == "display") {
        return FormatContextKind::track_display;
    }
    if (value == "now-playing") {
        return FormatContextKind::now_playing;
    }
    if (value == "list") {
        return FormatContextKind::list_item;
    }
    if (value == "queue") {
        return FormatContextKind::queue;
    }
    if (value == "path") {
        return FormatContextKind::path_generation;
    }
    if (value == "tree") {
        return FormatContextKind::tree_level;
    }
    if (value == "group") {
        return FormatContextKind::grouping;
    }
    if (value == "copy") {
        return FormatContextKind::copy_export;
    }
    if (value == "sort") {
        return FormatContextKind::sort;
    }
    if (value == "metadata-transform") {
        return FormatContextKind::metadata_transformation;
    }
    return std::nullopt;
}

[[nodiscard]] std::string escaped(const std::string_view value) {
    std::string result{"\""};
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U) {
                constexpr std::string_view digits = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits.at(character >> 4U));
                result.push_back(digits.at(character & 0x0FU));
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    result.push_back('"');
    return result;
}

class CommandLineContext final : public trackknife::titleformat::EvaluationContext {
  public:
    explicit CommandLineContext(const FormatContextKind kind) : kind_(kind) {}

    void add(std::string name, std::string value) {
        fields_.emplace_back(asciiLower(name), std::move(value));
    }

    void addTechnicalInfo(std::string name, std::string value) {
        technical_info_.emplace_back(asciiLower(name), std::move(value));
    }

    [[nodiscard]] FormatContextKind kind() const noexcept override { return kind_; }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match = std::ranges::find(fields_, normalized, &Field::first);
        return match == fields_.end() ? std::nullopt : std::optional<std::string>{match->second};
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        MetadataValues values;
        for (const auto& [field_name, value] : fields_) {
            if (field_name == normalized) {
                values.push_back(value);
            }
        }
        return values.empty() ? std::nullopt : std::optional<MetadataValues>{std::move(values)};
    }

    [[nodiscard]] std::optional<std::string>
    resolveTechnicalInfo(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match = std::ranges::find(technical_info_, normalized, &Field::first);
        return match == technical_info_.end() ? std::nullopt
                                              : std::optional<std::string>{match->second};
    }

  private:
    using Field = std::pair<std::string, std::string>;
    FormatContextKind kind_;
    std::vector<Field> fields_;
    std::vector<Field> technical_info_;
};

void printUsage(std::ostream& output) {
    output << "Usage: melody-titleformat [options] SCRIPT\n"
              "\n"
              "Options:\n"
              "  --field NAME=VALUE       Add an ordered metadata value; repeatable\n"
              "  --info NAME=VALUE        Add a technical information field; repeatable\n"
              "  --context NAME           display, now-playing, list, queue, path, tree,\n"
              "                           group, copy, sort, or metadata-transform\n"
              "  --show-dependencies      Print normalized field dependencies\n"
              "  --help                   Show this help\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    FormatContextKind context_kind = FormatContextKind::track_display;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::pair<std::string, std::string>> technical_info;
    std::optional<std::string> script;
    bool show_dependencies = false;
    bool positional_only = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (!positional_only && argument == "--") {
            positional_only = true;
        } else if (!positional_only && argument == "--help") {
            printUsage(std::cout);
            return EXIT_SUCCESS;
        } else if (!positional_only && argument == "--show-dependencies") {
            show_dependencies = true;
        } else if (!positional_only &&
                   (argument == "--field" || argument == "--info" || argument == "--context")) {
            if (index + 1 >= argc) {
                std::cerr << argument << " requires a value\n";
                return 2;
            }
            const std::string value{argv[++index]};
            if (argument == "--context") {
                const auto parsed = parseContext(value);
                if (!parsed) {
                    std::cerr << "unknown context: " << value << '\n';
                    return 2;
                }
                context_kind = *parsed;
            } else {
                const auto separator = value.find('=');
                if (separator == std::string::npos || separator == 0U) {
                    std::cerr << argument << " must have NAME=VALUE form\n";
                    return 2;
                }
                auto item = std::pair{value.substr(0, separator), value.substr(separator + 1U)};
                if (argument == "--field") {
                    fields.push_back(std::move(item));
                } else {
                    technical_info.push_back(std::move(item));
                }
            }
        } else if (!positional_only && argument.starts_with('-')) {
            std::cerr << "unknown option: " << argument << '\n';
            return 2;
        } else if (script) {
            std::cerr << "only one SCRIPT argument is accepted; quote scripts containing spaces\n";
            return 2;
        } else {
            script = std::string{argument};
        }
    }

    if (!script) {
        printUsage(std::cerr);
        return 2;
    }

    trackknife::titleformat::CompileOptions compile_options;
    compile_options.context = context_kind;
    auto compiled = trackknife::titleformat::compile(*script, std::move(compile_options));
    for (const auto& diagnostic : compiled.parse_diagnostics) {
        std::cerr << "parse error [" << diagnostic.span.begin << ',' << diagnostic.span.end
                  << "): " << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : compiled.diagnostics) {
        std::cerr << "compile error [" << diagnostic.span.begin << ',' << diagnostic.span.end
                  << "): " << diagnostic.message << '\n';
    }
    if (!compiled.program) {
        return 3;
    }

    CommandLineContext context(context_kind);
    for (auto& [name, value] : fields) {
        context.add(std::move(name), std::move(value));
    }
    for (auto& [name, value] : technical_info) {
        context.addTechnicalInfo(std::move(name), std::move(value));
    }
    if (compiled.program->hasExpansions()) {
        const auto results = trackknife::titleformat::evaluateExpanded(*compiled.program, context);
        if (!results) {
            std::cerr << "evaluation error: " << results.error().message << '\n';
            for (const auto& item : results.error().context) {
                std::cerr << "  " << item.key << '=' << item.value << '\n';
            }
            return 4;
        }
        for (std::size_t index = 0; index < results->size(); ++index) {
            std::cout << "text[" << index << "]=" << escaped(results->at(index).text) << '\n';
        }
    } else {
        const auto result = trackknife::titleformat::evaluate(*compiled.program, context);
        if (!result) {
            std::cerr << "evaluation error: " << result.error().message << '\n';
            for (const auto& item : result.error().context) {
                std::cerr << "  " << item.key << '=' << item.value << '\n';
            }
            return 4;
        }
        std::cout << "text=" << escaped(result->text) << '\n';
    }
    if (show_dependencies) {
        std::cout << "field_dependencies=";
        for (std::size_t index = 0; index < compiled.program->fieldDependencies().size(); ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << compiled.program->fieldDependencies().at(index);
        }
        std::cout << '\n';
        std::cout << "technical_dependencies=";
        for (std::size_t index = 0; index < compiled.program->technicalDependencies().size();
             ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << compiled.program->technicalDependencies().at(index);
        }
        std::cout << '\n';
        std::cout << "expansion_dependencies=";
        for (std::size_t index = 0; index < compiled.program->expansionDependencies().size();
             ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << compiled.program->expansionDependencies().at(index);
        }
        std::cout << '\n';
    }
    return EXIT_SUCCESS;
}
