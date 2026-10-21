#include "alchemy/darwin/mediaplayer/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// MediaPlayer: the Now Playing center keeps what it is given and the remote
// commands keep their targets, but the host never sends a remote command.
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::mediaplayer {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view media_player = "/System/Library/Frameworks/MediaPlayer.framework/Versions/A/MediaPlayer";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(media_player));
    const auto nothing = [](CpuState&, GuestMemory&) {};
    string(media_player, "_MPMediaItemPropertyTitle", "title");
    string(media_player, "_MPMediaItemPropertyAlbumTitle", "albumTitle");
    string(media_player, "_MPMediaItemPropertyArtist", "artist");
    string(media_player, "_MPMediaItemPropertyArtwork", "artwork");
    string(media_player, "_MPMediaItemPropertyPlaybackDuration", "playbackDuration");
    string(media_player, "_MPNowPlayingInfoPropertyElapsedPlaybackTime", "MPNowPlayingInfoPropertyElapsedPlaybackTime");
    string(media_player, "_MPNowPlayingInfoPropertyPlaybackRate", "MPNowPlayingInfoPropertyPlaybackRate");
    string(media_player, "_MPNowPlayingInfoPropertyCurrentPlaybackDate", "MPNowPlayingInfoPropertyCurrentPlaybackDate");
    const auto now_playing = cls(media_player, "MPNowPlayingInfoCenter", "NSObject");
    singleton(now_playing, "defaultCenter");
    object_properties(now_playing, {"nowPlayingInfo"});
    integer_properties(now_playing, {"playbackState"});
    const auto artwork = cls(media_player, "MPMediaItemArtwork", "NSObject");
    object_properties(artwork, {"requestHandler"});
    method(artwork, "initWithBoundsSize:requestHandler:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2]) {
            const auto block = bridge->block_copy(s.x[2]);
            store(s.x[0], "requestHandler", block);
            bridge->block_release(block);
        }
    }, false, "@@:{CGSize=dd}@?");
    const auto command = cls(media_player, "MPRemoteCommand", "NSObject");
    integer_properties(command, {"enabled"});
    method(command, "isEnabled", [this](CpuState& s, GuestMemory&) { s.x[0] = stored_integer(s.x[0], "enabled") != 0; }, false, "B@:");
    // Targets are kept so they can be removed; the host never sends a remote
    // command, so none is ever called.
    method(command, "addTarget:action:", nothing, false, "v@:@:");
    method(command, "removeTarget:action:", nothing, false, "v@:@:");
    method(command, "removeTarget:", nothing, false, "v@:@");
    method(command, "addTargetWithHandler:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(create(objc.find_class("NSObject")));   // the opaque target
    });
    const auto position_command = cls(media_player, "MPChangePlaybackPositionCommand", "MPRemoteCommand");
    static_cast<void>(position_command);
    const auto rate_command = cls(media_player, "MPChangePlaybackRateCommand", "MPRemoteCommand");
    object_properties(rate_command, {"supportedPlaybackRates"});
    const auto skip_command = cls(media_player, "MPSkipIntervalCommand", "MPRemoteCommand");
    object_properties(skip_command, {"preferredIntervals"});
    const auto repeat_command = cls(media_player, "MPChangeRepeatModeCommand", "MPRemoteCommand");
    integer_properties(repeat_command, {"currentRepeatType"});
    const auto shuffle_command = cls(media_player, "MPChangeShuffleModeCommand", "MPRemoteCommand");
    integer_properties(shuffle_command, {"currentShuffleType"});
    const auto command_center = cls(media_player, "MPRemoteCommandCenter", "NSObject");
    singleton(command_center, "sharedCommandCenter");
    const std::vector<std::pair<const char*, Address>> commands{
        {"playCommand", command}, {"pauseCommand", command}, {"stopCommand", command}, {"togglePlayPauseCommand", command},
        {"nextTrackCommand", command}, {"previousTrackCommand", command}, {"seekForwardCommand", command},
        {"seekBackwardCommand", command}, {"enableLanguageOptionCommand", command}, {"disableLanguageOptionCommand", command},
        {"changePlaybackPositionCommand", position_command}, {"changePlaybackRateCommand", rate_command},
        {"skipForwardCommand", skip_command}, {"skipBackwardCommand", skip_command},
        {"changeRepeatModeCommand", repeat_command}, {"changeShuffleModeCommand", shuffle_command},
        {"likeCommand", command}, {"dislikeCommand", command}, {"bookmarkCommand", command}, {"ratingCommand", command}};
    for (const auto& [selector, command_class] : commands) {
        method(command_center, selector, [this, key = std::string(selector), command_class](CpuState& s, GuestMemory&) {
            std::lock_guard lock(mutex);
            auto value = stored(s.x[0], key);
            if (!value) {
                const auto created = create(command_class);
                store_integer(created, "enabled", 1);
                store(s.x[0], key, created);
                release(created);
                value = stored(s.x[0], key);
            }
            s.x[0] = value;
        });
    }
}

} // namespace

void install_mediaplayer(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::mediaplayer
