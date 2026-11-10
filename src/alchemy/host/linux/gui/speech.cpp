#include "alchemy/host/speech.hpp"

namespace alchemy::host::speech {

Result<std::vector<Voice>> voices() { return Error::unsupported; }

Result<Speech> synthesize(std::string_view, std::u16string_view, const Options&) { return Error::unsupported; }

} // namespace alchemy::host::speech
