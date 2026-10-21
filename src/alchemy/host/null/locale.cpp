// Null host backend, area "locale": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/locale.hpp"

namespace alchemy::host::locale {

Result<std::u16string> normalize(std::u16string_view, Form) { return Error::unsupported; }
Result<int> compare(std::string_view, std::u16string_view, std::u16string_view, std::uint32_t) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> sort_key(std::string_view, std::u16string_view, std::uint32_t) { return Error::unsupported; }
Result<std::u16string> map(std::string_view, std::u16string_view, Mapping) { return Error::unsupported; }
Result<Match> find(std::string_view, std::u16string_view, std::u16string_view, Search, std::uint32_t) { return Error::unsupported; }
Status classify(std::u16string_view, std::span<std::uint16_t>) { return Error::unsupported; }
Result<std::string> decode_mac_roman(std::string_view) { return Error::unsupported; }
bool valid(std::string_view) { return false; }
Result<std::string> user_locale() { return Error::unsupported; }
Result<std::vector<std::string>> locales() { return Error::unsupported; }
Result<std::string> text(std::string_view, Text, unsigned) { return Error::unsupported; }
Result<std::uint32_t> number(std::string_view, Number) { return Error::unsupported; }
Result<std::string> format_number(std::string_view, std::string_view, const Format&) { return Error::unsupported; }
Result<std::string> format_currency(std::string_view, std::string_view, const Format&, std::string_view) { return Error::unsupported; }
Result<std::string> region_name(std::string_view) { return Error::unsupported; }
Result<std::string> currency_name(std::string_view, bool) { return Error::unsupported; }
Result<std::string> time_zone() { return Error::unsupported; }

} // namespace alchemy::host::locale
