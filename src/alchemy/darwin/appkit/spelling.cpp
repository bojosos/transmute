#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/host/locale.hpp"
#include "alchemy/host/ui.hpp"

#include <cctype>

// NSSpellChecker over the host's spell checker (host::ui; on win32 the
// Windows Spell Checking API, ISpellCheckerFactory, Windows 8 and later, on a
// dedicated COM host thread that owns one ISpellChecker per language).
// - Languages: the host's spell-checking languages, written as macOS does
//   ("en_US"); setLanguage: takes either separator or a bare language ("en"
//   picks the first "en-*" dictionary). The default is the user's locale
//   when the host can check it, else the first language.
// - Checking: checkSpellingOfString:..., checkString:range:types:...,
//   requestCheckingOfString:... (asynchronous, on a global queue) report the
//   words Windows marks misspelled (corrective action GET_SUGGESTIONS or
//   REPLACE; repeated words, DELETE, are grammar on macOS and not reported),
//   less the document's ignored words and the learned words.
//   guessesForWordRange: is Windows' suggestion list; correctionForWordRange:
//   its automatic replacement (REPLACE) or nil. Word counts count runs of
//   letters, digits and apostrophes.
// - Learned words (learnWord:, unlearnWord:, hasLearnedWord:) are the guest
//   user's ~/Library/Spelling/LocalDictionary, one word per line (the format
//   is from memory, unverified); the host's own dictionary is not touched.
// - Not available, said so: completions (an empty list), text candidates
//   (the completion handler gets an empty list), the Spelling and Grammar and
//   Substitutions panels (nil: no such panel exists here), grammar results,
//   orthographies (nil). Correction indicators are never shown: dismissing a
//   view's indicator calls its completion handler with nil (not accepted);
//   nothing prevents autocorrection and no autospace is added or removed.
// - The automatic substitution switches (+isAutomaticTextReplacementEnabled
//   and the others) read the NSAutomatic*Enabled keys of the global domain,
//   YES when unset.
namespace alchemy::gui::values {
namespace {

constexpr std::uint64_t not_found = 0x7fffffffffffffffULL;   // NSNotFound
constexpr std::uint64_t checking_spelling = 1ULL << 1;      // NSTextCheckingTypeSpelling

// UTF-8 <-> UTF-16 (NSString text and its ranges are UTF-16).
std::u16string wide(std::string_view text) { return alchemy::gui::appkit::drawing::utf16(text); }
std::string narrow(std::u16string_view text) { return alchemy::gui::appkit::drawing::utf8(text); }

using Misspelling = alchemy::host::ui::Misspelling;

// The host's spell checker (host::ui; on win32 a COM thread with one checker
// per language). A host without one lists no languages and finds nothing.
struct Engine {
    static Engine& instance() {
        static Engine engine;
        return engine;
    }
    std::vector<std::string> languages() { return alchemy::host::ui::spelling_languages().value_or({}); }
    std::vector<Misspelling> check(const std::string& language, const std::u16string& text) {
        return alchemy::host::ui::check_spelling(language, text).value_or({});
    }
    std::vector<std::u16string> suggest(const std::string& language, const std::u16string& word) {
        return alchemy::host::ui::spelling_suggestions(language, word).value_or({});
    }
};

// Language tags are ASCII.
bool same_tag(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string mac_language(std::string_view windows) {
    std::string text(windows);
    std::replace(text.begin(), text.end(), '-', '_');
    return text;
}

// Runs of letters, digits and apostrophes.
std::int64_t count_words(std::u16string_view text) {
    namespace locale = alchemy::host::locale;
    std::vector<std::uint16_t> classes(text.size());
    const bool classified = static_cast<bool>(locale::classify(text, classes));
    std::int64_t count = 0;
    bool inside = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto c = text[index];
        const bool word = (classified && (classes[index] & (locale::alpha | locale::digit))) || c == u'\'' || c == 0x2019;
        if (word && !inside) ++count;
        inside = word;
    }
    return count;
}

struct SpellState {
    std::mutex mutex;
    std::string language;                                  // the host's tag
    bool identifies_languages{true};
    std::int64_t next_tag{1};
    std::map<std::int64_t, std::set<std::u16string>> ignored;   // per document tag
    std::set<std::u16string> learned;
    bool learned_loaded{};
    std::filesystem::path dictionary;                      // host path of LocalDictionary
    std::int64_t next_sequence{1};
    Address shared{};
};

} // namespace

void Provider::install_spelling() {
    if (!objc.find_class("NSSpellChecker")) static_cast<void>(cls(fill::appkit, "NSSpellChecker", "NSObject"));
    auto state = std::make_shared<SpellState>();
    {
        std::string home;
        for (const auto& entry : process.environment)
            if (entry.rfind("HOME=", 0) == 0) home = entry.substr(5);
        if (!home.empty() && process.map_guest_path) state->dictionary = process.map_guest_path(home + "/Library/Spelling/LocalDictionary");
    }
    const auto load_learned = [state] {   // state->mutex held
        if (state->learned_loaded) return;
        state->learned_loaded = true;
        if (state->dictionary.empty()) return;
        std::ifstream file(state->dictionary, std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) state->learned.insert(wide(line));
        }
    };
    const auto save_learned = [state] {   // state->mutex held
        if (state->dictionary.empty()) return;
        std::error_code ignored;
        std::filesystem::create_directories(state->dictionary.parent_path(), ignored);
        std::ofstream file(state->dictionary, std::ios::binary | std::ios::trunc);
        for (const auto& word : state->learned) file << narrow(word) << '\n';
    };
    // The host's tag for a macOS or BCP 47 language name ("" when none).
    const auto resolve = [](std::string_view name) -> std::string {
        std::string tag(name);
        std::replace(tag.begin(), tag.end(), '_', '-');
        const auto available = Engine::instance().languages();
        for (const auto& language : available)
            if (same_tag(language, tag)) return language;
        for (const auto& language : available)
            if (language.size() > tag.size() && same_tag(std::string_view(language).substr(0, tag.size()), tag) && language[tag.size()] == '-')
                return language;
        return {};
    };
    const auto current = [state, resolve]() -> std::string {
        {
            std::lock_guard lock(state->mutex);
            if (!state->language.empty()) return state->language;
        }
        std::string chosen;
        if (const auto locale = alchemy::host::locale::user_locale()) chosen = resolve(*locale);
        if (chosen.empty()) {
            const auto available = Engine::instance().languages();
            if (!available.empty()) chosen = available.front();
        }
        std::lock_guard lock(state->mutex);
        if (state->language.empty()) state->language = chosen;
        return state->language;
    };
    // The language a call names (nil or unknown: the checker's language).
    const auto language_of = [this, resolve, current](Address name) {
        if (name) if (auto chosen = resolve(cf.utf8(name)); !chosen.empty()) return chosen;
        return current();
    };
    const auto text_of = [this](Address string) { return string ? wide(cf.utf8(string)) : std::u16string(); };
    // Misspellings of `text` the document and the user have not accepted.
    const auto misspellings = [state, load_learned](const std::string& language, const std::u16string& text, std::int64_t tag) {
        auto found = Engine::instance().check(language, text);
        std::lock_guard lock(state->mutex);
        load_learned();
        const auto ignored = state->ignored.find(tag);
        std::erase_if(found, [&](const Misspelling& entry) {
            const auto word = text.substr(entry.start, entry.length);
            return state->learned.contains(word) || (ignored != state->ignored.end() && ignored->second.contains(word));
        });
        return found;
    };

    // ---- NSTextCheckingResult spelling results ----
    if (!objc.find_class("NSTextCheckingResult")) static_cast<void>(cls(fill::foundation, "NSTextCheckingResult", "NSObject"));
    const auto spelling_result = cls(fill::foundation, "_AlchemySpellCheckingResult", "NSTextCheckingResult");
    cleanup_on_dealloc(spelling_result);
    method(spelling_result, "resultType", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(stored_integer(s.x[0], "type")); }, false, "Q@:");
    method(spelling_result, "range", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        s.x[0] = static_cast<Address>(stored_integer(self, "location"));
        s.x[1] = static_cast<Address>(stored_integer(self, "length"));
    }, false, "{_NSRange=QQ}@:");
    method(spelling_result, "numberOfRanges", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "Q@:");
    method(spelling_result, "rangeAtIndex:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (s.x[2] != 0) throw std::runtime_error("NSRangeException: -[NSTextCheckingResult rangeAtIndex:]: index out of bounds");
        s.x[0] = static_cast<Address>(stored_integer(self, "location"));
        s.x[1] = static_cast<Address>(stored_integer(self, "length"));
    }, false, "{_NSRange=QQ}@:Q");
    method(spelling_result, "copyWithZone:", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); }, false, "@@:^v");   // immutable
    const auto make_result = [this, spelling_result](std::uint64_t location, std::uint64_t length) {
        const auto result = create(spelling_result);
        store_integer(result, "type", static_cast<std::int64_t>(checking_spelling));
        store_integer(result, "location", static_cast<std::int64_t>(location));
        store_integer(result, "length", static_cast<std::int64_t>(length));
        return result;
    };
    extend("NSTextCheckingResult", "spellCheckingResultWithRange:", [this, make_result](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(make_result(s.x[2], s.x[3]));
    }, true, "@@:{_NSRange=QQ}");
    // An NSArray of spelling results for `text` (+1).
    const auto results_for = [this, make_result, misspellings](const std::string& language, const std::u16string& text,
                                                                std::uint64_t offset, std::int64_t tag) {
        std::vector<Address> results;
        for (const auto& entry : misspellings(language, text, tag)) results.push_back(make_result(offset + entry.start, entry.length));
        const auto list = cf.array(results);
        for (const auto result : results) release(result);
        return list;
    };

    // ---- NSSpellChecker ----
    extend("NSSpellChecker", "sharedSpellChecker", [this, state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (!state->shared) {
            state->shared = objc.send(objc.allocate(s.x[0]), "init").x0;
            if (const auto record = process.objects().find(state->shared)) record->immortal = true;
        }
        s.x[0] = state->shared;
    }, true);
    extend("NSSpellChecker", "sharedSpellCheckerExists", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        s.x[0] = state->shared != 0;
    }, true, "B@:");
    extend("NSSpellChecker", "availableLanguages", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& language : Engine::instance().languages()) names.push_back(cf.string(mac_language(language)));
        s.x[0] = bridge->autorelease(cf.array(names));
        for (const auto name : names) release(name);
    });
    extend("NSSpellChecker", "userPreferredLanguages", [this, current](CpuState& s, GuestMemory&) {
        const auto language = current();
        std::vector<Address> names;
        if (!language.empty()) names.push_back(cf.string(mac_language(language)));
        s.x[0] = bridge->autorelease(cf.array(names));
        for (const auto name : names) release(name);
    });
    extend("NSSpellChecker", "language", [this, current](CpuState& s, GuestMemory&) {
        const auto language = current();
        s.x[0] = bridge->autorelease(cf.string(mac_language(language)));
    });
    extend("NSSpellChecker", "setLanguage:", [this, state, resolve](CpuState& s, GuestMemory&) {
        const auto chosen = s.x[2] ? resolve(cf.utf8(s.x[2])) : std::string();
        if (!chosen.empty()) { std::lock_guard lock(state->mutex); state->language = chosen; }
        s.x[0] = !chosen.empty();
    }, false, "B@:@");
    extend("NSSpellChecker", "automaticallyIdentifiesLanguages", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        s.x[0] = state->identifies_languages;
    }, false, "B@:");
    extend("NSSpellChecker", "setAutomaticallyIdentifiesLanguages:", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        state->identifies_languages = (s.x[2] & 0xff) != 0;
    }, false, "v@:B");
    // +uniqueSpellDocumentTag is the public form; the instance one answers the same counter.
    for (const bool class_level : {true, false})
        extend("NSSpellChecker", "uniqueSpellDocumentTag", [state](CpuState& s, GuestMemory&) {
            std::lock_guard lock(state->mutex);
            s.x[0] = static_cast<Address>(state->next_tag++);
        }, class_level, "q@:");
    extend("NSSpellChecker", "closeSpellDocumentWithTag:", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        state->ignored.erase(static_cast<std::int64_t>(s.x[2]));
    }, false, "v@:q");
    extend("NSSpellChecker", "ignoreWord:inSpellDocumentWithTag:", [state, text_of](CpuState& s, GuestMemory&) {
        const auto word = text_of(s.x[2]);
        std::lock_guard lock(state->mutex);
        if (!word.empty()) state->ignored[static_cast<std::int64_t>(s.x[3])].insert(word);
    }, false, "v@:@q");
    extend("NSSpellChecker", "ignoredWordsInSpellDocumentWithTag:", [this, state](CpuState& s, GuestMemory&) {
        std::vector<Address> words;
        {
            std::lock_guard lock(state->mutex);
            if (const auto found = state->ignored.find(static_cast<std::int64_t>(s.x[2])); found != state->ignored.end())
                for (const auto& word : found->second) words.push_back(cf.string(narrow(word)));
        }
        s.x[0] = words.empty() ? 0 : bridge->autorelease(cf.array(words));
        for (const auto word : words) release(word);
    }, false, "@@:q");
    extend("NSSpellChecker", "setIgnoredWords:inSpellDocumentWithTag:", [this, state, text_of](CpuState& s, GuestMemory&) {
        std::set<std::u16string> words;
        for (const auto word : s.x[2] ? cf.array_values(s.x[2]) : std::vector<Address>{}) words.insert(text_of(word));
        std::lock_guard lock(state->mutex);
        state->ignored[static_cast<std::int64_t>(s.x[3])] = std::move(words);
    }, false, "v@:@q");
    extend("NSSpellChecker", "learnWord:", [state, text_of, load_learned, save_learned](CpuState& s, GuestMemory&) {
        const auto word = text_of(s.x[2]);
        if (word.empty()) return;
        std::lock_guard lock(state->mutex);
        load_learned();
        if (state->learned.insert(word).second) save_learned();
    }, false, "v@:@");
    extend("NSSpellChecker", "unlearnWord:", [state, text_of, load_learned, save_learned](CpuState& s, GuestMemory&) {
        const auto word = text_of(s.x[2]);
        std::lock_guard lock(state->mutex);
        load_learned();
        if (state->learned.erase(word)) save_learned();
    }, false, "v@:@");
    extend("NSSpellChecker", "hasLearnedWord:", [state, text_of, load_learned](CpuState& s, GuestMemory&) {
        const auto word = text_of(s.x[2]);
        std::lock_guard lock(state->mutex);
        load_learned();
        s.x[0] = state->learned.contains(word);
    }, false, "B@:@");
    // NSRange checkSpellingOfString:startingAt:language:wrap:inSpellDocumentWithTag:wordCount:
    const auto check_spelling = [this, language_of, text_of, misspellings](CpuState& s, Address string, std::uint64_t start, Address language,
                                                                         bool wrap, std::int64_t tag, Address word_count) {
        const auto text = text_of(string);
        const auto found = misspellings(language_of(language), text, tag);
        const Misspelling* chosen{};
        for (const auto& entry : found) if (entry.start >= start) { chosen = &entry; break; }
        if (!chosen && wrap) for (const auto& entry : found) if (entry.start < start) { chosen = &entry; break; }
        if (word_count) memory().write64(word_count, static_cast<std::uint64_t>(count_words(text)));
        s.x[0] = chosen ? chosen->start : not_found;
        s.x[1] = chosen ? chosen->length : 0;
    };
    extend("NSSpellChecker", "checkSpellingOfString:startingAt:language:wrap:inSpellDocumentWithTag:wordCount:", [check_spelling](CpuState& s, GuestMemory&) {
        check_spelling(s, s.x[2], s.x[3], s.x[4], (s.x[5] & 0xff) != 0, static_cast<std::int64_t>(s.x[6]), s.x[7]);
    }, false, "{_NSRange=QQ}@:@q@Bq^q");
    extend("NSSpellChecker", "checkSpellingOfString:startingAt:", [check_spelling](CpuState& s, GuestMemory&) {
        check_spelling(s, s.x[2], s.x[3], 0, false, 0, 0);
    }, false, "{_NSRange=QQ}@:@q");
    extend("NSSpellChecker", "countWordsInString:language:", [text_of](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(count_words(text_of(s.x[2])));
    }, false, "q@:@@");
    // guessesForWordRange:(NSRange)range inString:language:inSpellDocumentWithTag:
    extend("NSSpellChecker", "guessesForWordRange:inString:language:inSpellDocumentWithTag:", [this, language_of, text_of](CpuState& s, GuestMemory&) {
        const auto text = text_of(s.x[4]);
        const auto location = std::min<std::uint64_t>(s.x[2], text.size());
        const auto word = text.substr(static_cast<std::size_t>(location), static_cast<std::size_t>(std::min<std::uint64_t>(s.x[3], text.size() - location)));
        std::vector<Address> guesses;
        for (const auto& guess : Engine::instance().suggest(language_of(s.x[5]), word)) guesses.push_back(cf.string(narrow(guess)));
        s.x[0] = bridge->autorelease(cf.array(guesses));
        for (const auto guess : guesses) release(guess);
    }, false, "@@:{_NSRange=QQ}@@q");
    extend("NSSpellChecker", "correctionForWordRange:inString:language:inSpellDocumentWithTag:", [this, language_of, text_of](CpuState& s, GuestMemory&) {
        const auto text = text_of(s.x[4]);
        const auto location = std::min<std::uint64_t>(s.x[2], text.size());
        const auto word = text.substr(static_cast<std::size_t>(location), static_cast<std::size_t>(std::min<std::uint64_t>(s.x[3], text.size() - location)));
        Address result{};
        for (const auto& entry : Engine::instance().check(language_of(s.x[5]), word))
            if (entry.replace && !entry.replacement.empty()) { result = bridge->autorelease(cf.string(narrow(entry.replacement))); break; }
        s.x[0] = result;
    }, false, "@@:{_NSRange=QQ}@@q");
    extend("NSSpellChecker", "completionsForPartialWordRange:inString:language:inSpellDocumentWithTag:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(cf.array({}));   // the host has no completion dictionary
    }, false, "@@:{_NSRange=QQ}@@q");
    // checkString:range:types:options:inSpellDocumentWithTag:orthography:wordCount:
    // (x2 string, x3/x4 range, x5 types, x6 options, x7 tag; orthography and
    // wordCount pointers on the stack).
    extend("NSSpellChecker", "checkString:range:types:options:inSpellDocumentWithTag:orthography:wordCount:",
           [this, language_of, text_of, results_for](CpuState& s, GuestMemory& m) {
        const auto text = text_of(s.x[2]);
        const auto location = std::min<std::uint64_t>(s.x[3], text.size());
        const auto piece = text.substr(static_cast<std::size_t>(location), static_cast<std::size_t>(std::min<std::uint64_t>(s.x[4], text.size() - location)));
        const auto orthography = m.read64(s.sp), word_count = m.read64(s.sp + 8);
        if (orthography) m.write64(orthography, 0);
        if (word_count) m.write64(word_count, static_cast<std::uint64_t>(count_words(piece)));
        s.x[0] = (s.x[5] & checking_spelling) ? bridge->autorelease(results_for(language_of(0), piece, location, static_cast<std::int64_t>(s.x[7])))
                                              : bridge->autorelease(cf.array({}));
    }, false, "@@:@{_NSRange=QQ}Q@q^@^q");
    // requestCheckingOfString:range:types:options:inSpellDocumentWithTag:completionHandler:
    // -> sequence number; handler(sequence, results, orthography, wordCount) on a global queue.
    extend("NSSpellChecker", "requestCheckingOfString:range:types:options:inSpellDocumentWithTag:completionHandler:",
           [this, state, language_of, text_of, results_for](CpuState& s, GuestMemory& m) {
        const auto text = text_of(s.x[2]);
        const auto location = std::min<std::uint64_t>(s.x[3], text.size());
        const auto piece = text.substr(static_cast<std::size_t>(location), static_cast<std::size_t>(std::min<std::uint64_t>(s.x[4], text.size() - location)));
        const auto types = s.x[5];
        const auto tag = static_cast<std::int64_t>(s.x[7]);
        const auto handler = m.read64(s.sp);
        std::int64_t sequence{};
        {
            std::lock_guard lock(state->mutex);
            sequence = state->next_sequence++;
        }
        const auto language = language_of(0);
        if (handler) {
            const auto copied = bridge->block_copy(handler);
            const auto queue = call(fill::libsystem, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(queue, [this, copied, piece, location, types, tag, sequence, language, results_for] {
                const auto results = (types & checking_spelling) ? results_for(language, piece, location, tag) : cf.array({});
                try {
                    static_cast<void>(bridge->invoke_block(copied, {static_cast<Address>(sequence), results, 0, static_cast<Address>(count_words(piece))}));
                } catch (...) { release(results); bridge->block_release(copied); throw; }
                release(results);
                bridge->block_release(copied);
            });
        }
        s.x[0] = static_cast<Address>(sequence);
    }, false, "q@:@{_NSRange=QQ}Q@q@?");
    // requestCandidatesForSelectedRange:inString:types:options:inSpellDocumentWithTag:completionHandler:
    // -> sequence number; handler(sequence, candidates): none exist here.
    extend("NSSpellChecker", "requestCandidatesForSelectedRange:inString:types:options:inSpellDocumentWithTag:completionHandler:",
           [this, state](CpuState& s, GuestMemory& m) {
        const auto handler = m.read64(s.sp);
        std::int64_t sequence{};
        {
            std::lock_guard lock(state->mutex);
            sequence = state->next_sequence++;
        }
        if (handler) {
            const auto copied = bridge->block_copy(handler);
            const auto queue = call(fill::libsystem, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(queue, [this, copied, sequence] {
                const auto empty = cf.array({});
                try { static_cast<void>(bridge->invoke_block(copied, {static_cast<Address>(sequence), empty})); }
                catch (...) { release(empty); bridge->block_release(copied); throw; }
                release(empty);
                bridge->block_release(copied);
            });
        }
        s.x[0] = static_cast<Address>(sequence);
    }, false, "q@:{_NSRange=QQ}@Q@q@?");
    // Correction indicators (the autocorrection bubble) are never shown: the
    // completion handler is kept per view, and dismissing the view's
    // indicator calls it with nil (dismissed without accepting).
    // showCorrectionIndicatorOfType:(x2) primaryString:(x3) alternativeStrings:(x4)
    // forStringInRect:(d0-d3) view:(x5) completionHandler:(x6)
    auto indicators = std::make_shared<std::map<Address, Address>>();   // view -> copied handler
    const auto finish_indicator = [this, state, indicators](Address view) {
        Address handler{};
        {
            std::lock_guard lock(state->mutex);
            if (const auto found = indicators->find(view); found != indicators->end()) {
                handler = found->second;
                indicators->erase(found);
            }
        }
        if (!handler) return;
        try { static_cast<void>(bridge->invoke_block(handler, {0})); } catch (...) { bridge->block_release(handler); throw; }
        bridge->block_release(handler);
    };
    extend("NSSpellChecker", "showCorrectionIndicatorOfType:primaryString:alternativeStrings:forStringInRect:view:completionHandler:",
           [this, state, indicators, finish_indicator](CpuState& s, GuestMemory&) {
        const auto view = s.x[5];
        finish_indicator(view);   // one indicator per view: the previous one is dismissed
        if (s.x[6]) {
            std::lock_guard lock(state->mutex);
            (*indicators)[view] = bridge->block_copy(s.x[6]);
        }
    }, false, "v@:q@@{CGRect={CGPoint=dd}{CGSize=dd}}@@?");
    extend("NSSpellChecker", "dismissCorrectionIndicatorForView:", [finish_indicator](CpuState& s, GuestMemory&) { finish_indicator(s.x[2]); },
           false, "v@:@");
    // No autocorrection rules beyond Windows' replacements: nothing prevents
    // autocorrection, and no autospace is inserted or deleted.
    extend("NSSpellChecker", "preventsAutocorrectionBeforeString:language:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@@");
    extend("NSSpellChecker", "deletesAutospaceBetweenString:andString:language:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@@@");
    extend("NSSpellChecker", "insertsAutospaceAfterString:language:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@@");
    extend("NSSpellChecker", "recordResponse:toCorrection:forWord:language:inSpellDocumentWithTag:", [](CpuState&, GuestMemory&) {},
           false, "v@:q@@@q");
    extend("NSSpellChecker", "languageForWordRange:inString:orthography:", [this, current](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(cf.string(mac_language(current())));
    }, false, "@@:{_NSRange=QQ}@@");
    // No Spelling and Grammar or Substitutions panel exists here.
    extend("NSSpellChecker", "spellingPanel", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    extend("NSSpellChecker", "substitutionsPanel", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    extend("NSSpellChecker", "accessoryView", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    extend("NSSpellChecker", "updateSpellingPanelWithMisspelledWord:", [](CpuState&, GuestMemory&) {}, false, "v@:@");
    extend("NSSpellChecker", "updatePanels", [](CpuState&, GuestMemory&) {}, false, "v@:");
    // +isAutomatic...Enabled: the global domain's NSAutomatic...Enabled, YES when unset.
    for (const auto& [selector, key] : std::initializer_list<std::pair<const char*, const char*>>{
             {"isAutomaticTextReplacementEnabled", "NSAutomaticTextReplacementEnabled"},
             {"isAutomaticSpellingCorrectionEnabled", "NSAutomaticSpellingCorrectionEnabled"},
             {"isAutomaticQuoteSubstitutionEnabled", "NSAutomaticQuoteSubstitutionEnabled"},
             {"isAutomaticDashSubstitutionEnabled", "NSAutomaticDashSubstitutionEnabled"},
             {"isAutomaticCapitalizationEnabled", "NSAutomaticCapitalizationEnabled"},
             {"isAutomaticPeriodSubstitutionEnabled", "NSAutomaticPeriodSubstitutionEnabled"},
             {"isAutomaticTextCompletionEnabled", "NSAutomaticTextCompletionEnabled"},
             {"isAutomaticInlinePredictionEnabled", "NSAutomaticInlinePredictionEnabled"}}) {
        const std::string name(key);
        extend("NSSpellChecker", selector, [this, name](CpuState& s, GuestMemory& m) {
            const auto any_application = symbol(fill::core_foundation, "_kCFPreferencesAnyApplication");
            if (!any_application) { s.x[0] = 1; return; }
            const auto key_string = cf.string(name);
            const auto exists = process.allocate_guest(1);
            m.write8(exists, 0);
            const auto value = call(fill::core_foundation, "_CFPreferencesGetAppBooleanValue", {key_string, m.read64(any_application), exists});
            const bool set = m.read8(exists) != 0;
            process.release_guest(exists);
            release(key_string);
            s.x[0] = set ? (value & 0xff) != 0 : 1;
        }, true, "B@:");
    }
}

} // namespace alchemy::gui::values
