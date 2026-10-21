#pragma once

// NSRegularExpression's engine (host-only, no ICU): a backtracking matcher
// over UTF-16 text with ICU pattern syntax for the constructs below.
//   Literals and escapes: \t \n \r \f \a \e \0ooo \xhh \x{h..} \uhhhh \cX,
//     \Q...\E, escaped metacharacters.
//   Classes: . [...] [^...] ranges, nested escapes, \d \D \w \W \s \S \h \H
//     \v \V, POSIX [[:alpha:]] style names inside brackets.
//   Anchors: ^ $ \A \z \Z \b \B \G.
//   Groups: (...) (?:...) (?<name>...) (?>...) (?=...) (?!...) (?<=...)
//     (?<!...) (bounded look-behind), inline flags (?ismwx-ismwx) and
//     (?flags:...), comments (?#...).
//   Quantifiers: * + ? {n} {n,} {n,m}, lazy (?) and possessive (+) forms.
//   Back references \1..\99 and \k<name>.
// Not provided (Status::unsupported): Unicode properties (\p, \P, \N{...}),
// \X, class set operations ([a-z&&[^aeiou]], [a-z--[aeiou]]) and the
// \R/\O line constructs. Case-insensitive matching folds ASCII, Latin-1,
// Latin Extended-A, Greek and Cyrillic simple case pairs.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::regex {

// NSRegularExpressionOptions.
enum Option : std::uint64_t {
    case_insensitive = 1, allow_comments = 2, ignore_metacharacters = 4, dot_matches_line_separators = 8,
    anchors_match_lines = 16, unix_line_separators = 32, unicode_word_boundaries = 64,
};
// NSMatchingOptions.
enum MatchOption : std::uint64_t {
    report_progress = 1, report_completion = 2, anchored = 4, transparent_bounds = 8, without_anchoring_bounds = 16,
};

struct Range { std::int64_t location{-1}; std::int64_t length{0}; };   // location -1: NSNotFound

struct Match {
    std::vector<Range> groups;   // [0] the whole match; unmatched groups have location -1
};

enum class Status { ok, invalid, unsupported };

class Pattern {
public:
    ~Pattern();
    Pattern(Pattern&&) noexcept;
    Pattern& operator=(Pattern&&) noexcept;
    // Compiles `pattern`; on failure, `status` is invalid or unsupported and
    // `message` says why.
    static std::optional<Pattern> compile(std::u16string_view pattern, std::uint64_t options, Status& status, std::string& message);

    [[nodiscard]] std::size_t capture_count() const;   // excluding group 0
    [[nodiscard]] std::optional<std::size_t> group_named(std::u16string_view name) const;
    // The first match within [range.location, location + length) at or after
    // `from` (an absolute index inside the range).
    [[nodiscard]] std::optional<Match> search(std::u16string_view text, Range range, std::int64_t from, std::uint64_t match_options) const;
    // Every match inside `range`, left to right; an empty match advances the
    // next search by one character.
    [[nodiscard]] std::vector<Match> all(std::u16string_view text, Range range, std::uint64_t match_options) const;

private:
    struct Program;
    explicit Pattern(std::unique_ptr<Program> program);
    std::unique_ptr<Program> program_;
};

// +[NSRegularExpression escapedPatternForString:]: backslash before each
// metacharacter.
[[nodiscard]] std::u16string escape_pattern(std::u16string_view text);
// +escapedTemplateForString: ($ and \ escaped).
[[nodiscard]] std::u16string escape_template(std::u16string_view text);
// -replacementStringForResult:...withTemplate: ($n, ${name}, \x).
[[nodiscard]] std::u16string expand_template(const Pattern& pattern, std::u16string_view text, const Match& match, std::u16string_view replacement);

} // namespace alchemy::gui::completion_kit::regex
