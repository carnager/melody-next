// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include "trackknife/core/cancellation.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

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

class TestContext final : public trackknife::titleformat::EvaluationContext {
  public:
    explicit TestContext(trackknife::titleformat::FormatContextKind kind =
                             trackknife::titleformat::FormatContextKind::track_display)
        : kind_(kind) {}

    void add(std::string name, std::string value) {
        fields_.emplace_back(asciiLower(name), std::move(value));
    }

    void addMetadata(std::string name, std::vector<std::string> values) {
        metadata_.emplace_back(asciiLower(name), std::move(values));
    }

    void addTechnicalInfo(std::string name, std::string value) {
        technical_info_.emplace_back(asciiLower(name), std::move(value));
    }

    [[nodiscard]] trackknife::titleformat::FormatContextKind kind() const noexcept override {
        return kind_;
    }

    [[nodiscard]] std::optional<std::string>
    resolveField(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        lookups_.push_back(normalized);
        const auto match = std::ranges::find(fields_, normalized, &Field::first);
        return match == fields_.end() ? std::nullopt : std::optional<std::string>{match->second};
    }

    [[nodiscard]] std::optional<MetadataValues>
    resolveMetadata(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        lookups_.push_back(normalized);
        const auto match = std::ranges::find(metadata_, normalized, &MetadataField::first);
        if (match != metadata_.end()) {
            return match->second;
        }
        const auto field = std::ranges::find(fields_, normalized, &Field::first);
        if (field == fields_.end()) {
            return std::nullopt;
        }
        return MetadataValues{field->second};
    }

    [[nodiscard]] std::optional<std::string>
    resolveTechnicalInfo(const std::string_view name) const override {
        const auto normalized = asciiLower(name);
        const auto match = std::ranges::find(technical_info_, normalized, &Field::first);
        return match == technical_info_.end() ? std::nullopt
                                              : std::optional<std::string>{match->second};
    }

    [[nodiscard]] bool lookedUp(const std::string_view name) const {
        return std::ranges::find(lookups_, asciiLower(name)) != lookups_.end();
    }

  private:
    using Field = std::pair<std::string, std::string>;
    using MetadataField = std::pair<std::string, MetadataValues>;
    trackknife::titleformat::FormatContextKind kind_;
    std::vector<Field> fields_;
    std::vector<MetadataField> metadata_;
    std::vector<Field> technical_info_;
    mutable std::vector<std::string> lookups_;
};

[[nodiscard]] trackknife::core::Result<trackknife::titleformat::EvalValue>
evaluateScript(const std::string& source, const TestContext& context,
               trackknife::titleformat::EvaluationOptions options = {}) {
    trackknife::titleformat::CompileOptions compile_options;
    compile_options.context = context.kind();
    const auto compiled = trackknife::titleformat::compile(source, compile_options);
    if (!compiled.program) {
        return std::unexpected(trackknife::core::Error{
            trackknife::core::ErrorCode::invalid_argument, "test source did not compile", {}});
    }
    return trackknife::titleformat::evaluate(*compiled.program, context, std::move(options));
}

void fieldsUseSimplePredictableSemantics() {
    TestContext context;
    context.add("artist", "Knife Artist");
    context.addMetadata("genre", {"Ambient", "Drone"});
    context.addMetadata("empty", {""});

    const auto value = evaluateScript("By %ARTIST% [%missing%] %genre%", context);
    CHECK(value && value->text == "By Knife Artist [] Ambient; Drone");

    const auto literal_zero = evaluateScript("$if(0,yes,no)", context);
    CHECK(literal_zero && literal_zero->text == "yes");
    const auto missing = evaluateScript("$if(%missing%,yes,no)", context);
    CHECK(missing && missing->text == "no");
    const auto present_empty = evaluateScript("$if(%empty%,yes,no)", context);
    CHECK(present_empty && present_empty->text == "no");
}

void lazyFunctionsDoNotEvaluateUnselectedFields() {
    TestContext context;
    context.add("condition", "1");
    context.add("selected", "chosen");
    context.add("unselected", "wrong");
    const auto selected = evaluateScript("$if(%condition%,%selected%,%unselected%)", context);
    CHECK(selected && selected->text == "chosen");
    CHECK(context.lookedUp("condition"));
    CHECK(context.lookedUp("selected"));
    CHECK(!context.lookedUp("unselected"));

    TestContext fallback;
    fallback.add("second", "winner");
    fallback.add("third", "wrong");
    const auto coalesced = evaluateScript("$if2(%missing%,%second%,%third%)", fallback);
    CHECK(coalesced && coalesced->text == "winner");
    CHECK(!fallback.lookedUp("third"));

    const auto boolean = evaluateScript("$and(1,x)|$or(,x)|$not(x)|$and()|$or()", context);
    CHECK(boolean && boolean->text == "1|1||1|");
}

void integerFunctionsAreStrictBoundedAndDocumented() {
    TestContext context;
    const auto arithmetic = evaluateScript(
        "$add()|$mul()|$add(2,3,-1)|$sub(10,3,2)|$mul(3,4)|$div(-7,2)|$mod(-7,3)", context);
    CHECK(arithmetic && arithmetic->text == "0|1|4|5|12|-3|-1");

    const auto invalid_becomes_zero = evaluateScript("$add(4.8,c3po,+2)", context);
    CHECK(invalid_becomes_zero && invalid_becomes_zero->text == "2");
    const auto comparisons = evaluateScript("$gt(5,4)$gte(5,5)$lt(-2,0)$lte(1,0)", context);
    CHECK(comparisons && comparisons->text == "111");
    const auto padded = evaluateScript("$num(7,3)|$num(-7,4)", context);
    CHECK(padded && padded->text == "007|-007");

    const auto divide_by_zero = evaluateScript("$div(7,0)", context);
    CHECK(!divide_by_zero);
    CHECK(!divide_by_zero &&
          divide_by_zero.error().code == trackknife::core::ErrorCode::invalid_argument);

    // $decimal: the one fraction, strict at its edges as $div is.
    const auto decimals =
        evaluateScript("$decimal(3,5,1)|$decimal(-85,100,1)|$decimal(-1,100,1)|$decimal(1,3,0)|"
                       "$decimal(-9223372036854775808,1,0)|$decimal(7,2,9)",
                       context);
    CHECK(decimals && decimals->text == "0.6|-0.9|0.0|0|-9223372036854775808|3.500000000");
    const auto decimal_by_zero = evaluateScript("$decimal(7,0,1)", context);
    CHECK(!decimal_by_zero &&
          decimal_by_zero.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(!evaluateScript("$decimal(1,2,-1)", context));
    CHECK(!evaluateScript("$decimal(1,2,10)", context));
    // Scaling past the integer range is an error, never a wrapped number.
    CHECK(!evaluateScript("$decimal(9223372036854775807,1,1)", context));
}

void textFunctionsCountUnicodeScalars() {
    TestContext context;
    const auto value =
        evaluateScript("$lower(ÄBC)|$upper(äbc)|$len(A😀é)|$left(A😀é,2)|$right(A😀é,2)|"
                       "$longest(a,😀😀,bbb)|$repeat(é,3)|$replace(abba,b,x,a,y)|$pad(é,3,·)",
                       context);
    CHECK(value && value->text == "äbc|ÄBC|3|A😀|😀é|bbb|ééé|yxxy|é··");

    const auto escaped = evaluateScript("literal \\, \\$ \\% \\( \\) \\\\", context);
    CHECK(escaped && escaped->text == "literal , $ % ( ) \\");

    const auto bad_fill = evaluateScript("$pad(x,3,ab)", context);
    CHECK(!bad_fill);
}

void exposesOrderedMetadataAndTechnicalInformation() {
    TestContext context;
    context.addMetadata("artist", {"One", "Two", ""});
    context.addTechnicalInfo("channels", "2");
    const auto value =
        evaluateScript("%artist%|$get(artist)|$getmulti(artist,1)|$getmulti(artist,-1)|"
                       "$join(artist, + )|$lenmulti(artist)|$lenmulti(missing)|$info(channels)",
                       context);
    CHECK(value && value->text == "One; Two; |One; Two; |Two||One + Two + |3|0|2");
}

void coversRemainingBuiltinOverloadsAndErrors() {
    TestContext context;
    const auto value =
        evaluateScript("[$if(,x)]|$eq(a,a)$ne(a,b)$eqi(Ä,ä)|$min(3,-2,4)|$max(3,-2,4)|"
                       "$sub(4)|[$trim( \t x \n)]|$pad(x,3)|$replace(aaaa,aa,b,b,c)",
                       context);
    CHECK(value && value->text == "[]|111|-2|4|4|[x]|x  |cc");

    const auto negative_slice = evaluateScript("$left(text,-1)", context);
    CHECK(!negative_slice &&
          negative_slice.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto empty_search = evaluateScript("$replace(text,,x)", context);
    CHECK(!empty_search &&
          empty_search.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto overflow = evaluateScript("$add(9223372036854775807,1)", context);
    CHECK(!overflow && overflow.error().code == trackknife::core::ErrorCode::invalid_argument);
}

void enforcesHostLimitsCancellationAndBatchIsolation() {
    using namespace trackknife;
    TestContext context;
    auto output_limit = titleformat::EvaluationOptions{};
    output_limit.maximum_steps = 100U;
    output_limit.maximum_output_bytes = 7U;
    const auto output_limited = evaluateScript("$repeat(ab,4)", context, output_limit);
    CHECK(!output_limited);
    CHECK(!output_limited && output_limited.error().code == core::ErrorCode::limit_exceeded);

    auto step_limit = titleformat::EvaluationOptions{};
    step_limit.maximum_steps = 2U;
    const auto step_limited = evaluateScript("$add(1,2,3)", context, step_limit);
    CHECK(!step_limited);

    core::CancellationSource source;
    source.request_cancellation();
    auto cancellation_options = titleformat::EvaluationOptions{};
    cancellation_options.maximum_steps = 100U;
    cancellation_options.maximum_output_bytes = 100U;
    cancellation_options.cancellation = source.token();
    const auto cancelled = evaluateScript("%artist%", context, cancellation_options);
    CHECK(!cancelled && cancelled.error().code == core::ErrorCode::cancelled);

    auto compiled = titleformat::compile("%artist%");
    CHECK(compiled.program.has_value());
    if (!compiled.program) {
        return;
    }
    TestContext queue_context(titleformat::FormatContextKind::queue);
    const auto wrong_host = titleformat::evaluate(*compiled.program, queue_context);
    CHECK(!wrong_host && wrong_host.error().code == core::ErrorCode::invalid_argument);

    TestContext first;
    first.add("artist", "One");
    TestContext second;
    second.add("artist", "Two");
    const std::array<titleformat::EvaluationContextRef, 2> contexts{std::cref(first),
                                                                    std::cref(second)};
    const auto batch = titleformat::evaluateBatch(*compiled.program, std::span{contexts});
    CHECK(batch.size() == 2U);
    CHECK(batch.at(0) && batch.at(0)->text == "One");
    CHECK(batch.at(1) && batch.at(1)->text == "Two");

    std::array<std::future<core::Result<titleformat::EvalValue>>, 8> futures;
    for (std::size_t index = 0; index < futures.size(); ++index) {
        futures.at(index) = std::async(std::launch::async, [&program = *compiled.program, index]() {
            TestContext thread_context;
            thread_context.add("artist", "Thread " + std::to_string(index));
            return titleformat::evaluate(program, thread_context);
        });
    }
    for (std::size_t index = 0; index < futures.size(); ++index) {
        const auto result = futures.at(index).get();
        CHECK(result && result->text == "Thread " + std::to_string(index));
    }
}

void expandsTreeValuesExplicitlyAndBoundedly() {
    using namespace trackknife::titleformat;
    TestContext context(FormatContextKind::tree_level);
    context.addMetadata("genre", {"Ambient", "Drone"});
    context.addMetadata("mood", {"Calm", "Dark"});

    CompileOptions options;
    options.context = FormatContextKind::tree_level;
    const auto compiled = compile("$upper($each(genre)) / $each(mood)", options);
    CHECK(compiled.program.has_value());
    if (!compiled.program) {
        return;
    }
    const auto scalar = evaluate(*compiled.program, context);
    CHECK(!scalar);

    const auto expanded = evaluateExpanded(*compiled.program, context);
    CHECK(expanded && expanded->size() == 4U);
    if (expanded && expanded->size() == 4U) {
        CHECK(expanded->at(0).text == "AMBIENT / Calm");
        CHECK(expanded->at(1).text == "AMBIENT / Dark");
        CHECK(expanded->at(2).text == "DRONE / Calm");
        CHECK(expanded->at(3).text == "DRONE / Dark");
    }

    const auto repeated = compile("$each(genre)|$each(genre)", options);
    CHECK(repeated.program.has_value());
    if (repeated.program) {
        const auto values = evaluateExpanded(*repeated.program, context);
        CHECK(values && values->size() == 2U);
        CHECK(values && values->at(0).text == "Ambient|Ambient");
        CHECK(values && values->at(1).text == "Drone|Drone");
    }

    const auto fallback = compile("$if2($each(grouping),Unknown)", options);
    CHECK(fallback.program.has_value());
    if (fallback.program) {
        const auto values = evaluateExpanded(*fallback.program, context);
        CHECK(values && values->size() == 1U && values->front().text == "Unknown");
    }

    auto limited_options = EvaluationOptions{};
    limited_options.maximum_expanded_results = 3U;
    const auto limited = evaluateExpanded(*compiled.program, context, limited_options);
    CHECK(!limited && limited.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

} // namespace

int main() {
    fieldsUseSimplePredictableSemantics();
    lazyFunctionsDoNotEvaluateUnselectedFields();
    integerFunctionsAreStrictBoundedAndDocumented();
    textFunctionsCountUnicodeScalars();
    exposesOrderedMetadataAndTechnicalInformation();
    coversRemainingBuiltinOverloadsAndErrors();
    enforcesHostLimitsCancellationAndBatchIsolation();
    expandsTreeValuesExplicitlyAndBoundedly();
    return failures == 0 ? 0 : 1;
}
