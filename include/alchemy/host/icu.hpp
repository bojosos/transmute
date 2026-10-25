#pragma once

// Host area "icu" (owner: fw-foundation; the frameworks wave). The host's own
// ICU4C, through its stable C API: converters, transliterators, break
// iterators, case mapping, and (added as the frameworks need them) calendars,
// formatters, collation and regular expressions. CoreFoundation and Foundation
// are ICU-based on macOS, so calling ICU gives their semantics. The rules are
// in include/alchemy/host/host.hpp.
//
// ICU is a library with the same C API on every host, not an OS interface, so
// the macOS layer calls it directly through this table; only the backend knows
// where it comes from:
// - win32: the system ICU of Windows 10 1903 and later (System32\icu.dll,
//   unversioned export names);
// - null: no ICU (api() is nullptr).
// A Linux backend would load libicuuc/libicui18n and resolve the names with the
// library's version suffix (ucnv_open_74).
//
// Each entry is resolved on its own, so an older ICU leaves the functions it
// lacks null; a caller checks the entry it uses (or uses require()).
// Signatures and constants are those of ICU's public C headers (ICU license).
// Add a function by adding one X(...) line; the backend resolves it by name.

#include <cstdint>

namespace alchemy::host::icu {

using UChar = char16_t;
using UChar32 = std::int32_t;
using UErrorCode = std::int32_t;   // > 0 failure, <= 0 success or warning
using UBool = std::int8_t;

// Opaque ICU objects.
struct UConverter;
struct UTransliterator;
struct UBreakIterator;
struct UConverterToUnicodeArgs;
struct UConverterFromUnicodeArgs;

using UConverterToUCallback = void (*)(const void* context, UConverterToUnicodeArgs* args, const char* units,
                                       std::int32_t length, std::int32_t reason, UErrorCode* error);
using UConverterFromUCallback = void (*)(const void* context, UConverterFromUnicodeArgs* args, const UChar* units,
                                         std::int32_t length, UChar32 code_point, std::int32_t reason, UErrorCode* error);

// utypes.h
inline constexpr UErrorCode zero_error = 0;
inline constexpr UErrorCode string_not_terminated_warning = -124;
inline constexpr UErrorCode invalid_char_found = 10, truncated_char_found = 11, illegal_char_found = 12;
inline constexpr UErrorCode buffer_overflow_error = 15;
[[nodiscard]] constexpr bool failure(UErrorCode code) noexcept { return code > zero_error; }
// utrans.h UTransDirection
inline constexpr std::int32_t trans_forward = 0, trans_reverse = 1;
// ubrk.h UBreakIteratorType and UBRK_DONE
inline constexpr std::int32_t break_character = 0, break_word = 1, break_line = 2, break_sentence = 3;
inline constexpr std::int32_t break_done = -1;
// ubrk.h UWordBreak rule-status ranges: [none, number, letter, kana, ideo) start at 0, 100, 200, 300, 400.
inline constexpr std::int32_t word_none_limit = 100, word_number_limit = 200, word_letter_limit = 300,
    word_kana_limit = 400, word_ideo_limit = 500;
// uchar.h u_strFoldCase options
inline constexpr std::uint32_t fold_case_default = 0, fold_case_exclude_special_i = 1;

#define ALCHEMY_HOST_ICU_FUNCTIONS(X)                                                                                  \
    X(u_errorName, const char*, (UErrorCode code))                                                                     \
    X(u_getVersion, void, (std::uint8_t version[4]))                                                                   \
    /* ustring.h */                                                                                                    \
    X(u_strToUpper, std::int32_t, (UChar * dest, std::int32_t capacity, const UChar* source, std::int32_t length,      \
                                   const char* locale, UErrorCode* error))                                             \
    X(u_strToLower, std::int32_t, (UChar * dest, std::int32_t capacity, const UChar* source, std::int32_t length,      \
                                   const char* locale, UErrorCode* error))                                             \
    X(u_strToTitle, std::int32_t, (UChar * dest, std::int32_t capacity, const UChar* source, std::int32_t length,      \
                                   UBreakIterator* words, const char* locale, UErrorCode* error))                      \
    X(u_strFoldCase, std::int32_t, (UChar * dest, std::int32_t capacity, const UChar* source, std::int32_t length,     \
                                    std::uint32_t options, UErrorCode* error))                                         \
    /* ucnv.h */                                                                                                       \
    X(ucnv_open, UConverter*, (const char* name, UErrorCode* error))                                                   \
    X(ucnv_close, void, (UConverter * converter))                                                                      \
    X(ucnv_reset, void, (UConverter * converter))                                                                      \
    X(ucnv_getAlias, const char*, (const char* alias, std::uint16_t n, UErrorCode* error))                             \
    X(ucnv_countAliases, std::uint16_t, (const char* alias, UErrorCode* error))                                        \
    X(ucnv_getStandardName, const char*, (const char* name, const char* standard, UErrorCode* error))                  \
    X(ucnv_getMaxCharSize, std::int8_t, (const UConverter* converter))                                                 \
    X(ucnv_setToUCallBack, void, (UConverter * converter, UConverterToUCallback action, const void* context,           \
                                  UConverterToUCallback* old_action, const void** old_context, UErrorCode* error))     \
    X(ucnv_setFromUCallBack, void, (UConverter * converter, UConverterFromUCallback action, const void* context,       \
                                    UConverterFromUCallback* old_action, const void** old_context, UErrorCode* error)) \
    X(UCNV_TO_U_CALLBACK_STOP, void, (const void* context, UConverterToUnicodeArgs* args, const char* units,           \
                                      std::int32_t length, std::int32_t reason, UErrorCode* error))                    \
    X(UCNV_FROM_U_CALLBACK_STOP, void, (const void* context, UConverterFromUnicodeArgs* args, const UChar* units,      \
                                        std::int32_t length, UChar32 code_point, std::int32_t reason,                  \
                                        UErrorCode* error))                                                            \
    X(ucnv_setSubstChars, void, (UConverter * converter, const char* characters, std::int8_t length, UErrorCode* error)) \
    X(ucnv_toUChars, std::int32_t, (UConverter * converter, UChar * dest, std::int32_t capacity, const char* source,   \
                                    std::int32_t length, UErrorCode* error))                                           \
    X(ucnv_fromUnicode, void, (UConverter * converter, char** target, const char* target_limit, const UChar** source,  \
                               const UChar* source_limit, std::int32_t* offsets, UBool flush, UErrorCode* error))      \
    /* utrans.h */                                                                                                     \
    X(utrans_openU, UTransliterator*, (const UChar* id, std::int32_t id_length, std::int32_t direction,                \
                                       const UChar* rules, std::int32_t rules_length, void* parse_error,               \
                                       UErrorCode* error))                                                             \
    X(utrans_close, void, (UTransliterator * transliterator))                                                          \
    X(utrans_transUChars, void, (const UTransliterator* transliterator, UChar* text, std::int32_t* length,             \
                                 std::int32_t capacity, std::int32_t start, std::int32_t* limit, UErrorCode* error))   \
    /* ubrk.h */                                                                                                       \
    X(ubrk_open, UBreakIterator*, (std::int32_t type, const char* locale, const UChar* text, std::int32_t length,      \
                                   UErrorCode* error))                                                                 \
    X(ubrk_close, void, (UBreakIterator * iterator))                                                                   \
    X(ubrk_setText, void, (UBreakIterator * iterator, const UChar* text, std::int32_t length, UErrorCode* error))      \
    X(ubrk_first, std::int32_t, (UBreakIterator * iterator))                                                           \
    X(ubrk_last, std::int32_t, (UBreakIterator * iterator))                                                            \
    X(ubrk_next, std::int32_t, (UBreakIterator * iterator))                                                            \
    X(ubrk_previous, std::int32_t, (UBreakIterator * iterator))                                                        \
    X(ubrk_current, std::int32_t, (const UBreakIterator* iterator))                                                    \
    X(ubrk_following, std::int32_t, (UBreakIterator * iterator, std::int32_t offset))                                  \
    X(ubrk_preceding, std::int32_t, (UBreakIterator * iterator, std::int32_t offset))                                  \
    X(ubrk_isBoundary, UBool, (UBreakIterator * iterator, std::int32_t offset))                                        \
    X(ubrk_getRuleStatus, std::int32_t, (UBreakIterator * iterator))

struct Api {
#define ALCHEMY_HOST_ICU_ENTRY(name, result, parameters) result(*name) parameters = nullptr;
    ALCHEMY_HOST_ICU_FUNCTIONS(ALCHEMY_HOST_ICU_ENTRY)
#undef ALCHEMY_HOST_ICU_ENTRY
    std::uint8_t version[4]{};   // the loaded ICU's version (72.1.0.0)
};

// The host's ICU, loaded on first use (thread-safe); nullptr when the host has none.
[[nodiscard]] const Api* api() noexcept;

} // namespace alchemy::host::icu
