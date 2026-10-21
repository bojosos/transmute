// MediaPlayer (src/alchemy/darwin/mediaplayer/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/MediaPlayer.framework/Versions/A/MediaPlayer"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case mediaplayer_case("mediaplayer", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "MediaPlayer identity");
    const auto commands = f.send(f.cls("MPRemoteCommandCenter"), "sharedCommandCenter");
    const auto play = f.send(commands, "playCommand");
    require(play && play == f.send(commands, "playCommand") && f.send(play, "isEnabled") == 1, "play command");
    const auto center = f.send(f.cls("MPNowPlayingInfoCenter"), "defaultCenter");
    require(center && center == f.send(f.cls("MPNowPlayingInfoCenter"), "defaultCenter"), "now playing center singleton");
});

#undef LIBRARY
} // namespace
