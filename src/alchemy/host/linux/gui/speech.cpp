// Linux host backend, area "speech" (include/alchemy/host/speech.hpp): unsupported. The area renders speech into
// PCM samples with word times; Linux's speech services either speak straight to the audio output
// (speech-dispatcher) or are GPL synthesizer libraries (eSpeak NG) that a permissively licensed program does not
// link. So no voices are listed and nothing is synthesized: every call answers Error::unsupported, as a host
// without speech synthesis.
#include "alchemy/host/speech.hpp"

namespace alchemy::host::speech {

Result<std::vector<Voice>> voices() { return Error::unsupported; }
Result<Speech> synthesize(std::string_view, std::u16string_view, const Options&) { return Error::unsupported; }

} // namespace alchemy::host::speech
