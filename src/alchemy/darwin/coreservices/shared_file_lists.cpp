#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

#include <algorithm>
#include <iterator>
#include <string>

// CoreServices LSSharedFileList: a headless process has no Finder sidebar,
// login items or recent lists, so every list is empty and its seed stays 0.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

// An LSSharedFileListRef. A headless process has no Finder sidebar, login
// items or recent lists, so every list is empty and its seed stays 0.
struct SharedFileList final : ObjectPayload { std::string type; };
// LSSharedFileList.h list type identifiers.
constexpr std::string_view shared_file_list_types[] = {"com.apple.LSSharedFileList.FavoriteItems",
    "com.apple.LSSharedFileList.FavoriteVolumes", "com.apple.LSSharedFileList.RecentApplicationItems",
    "com.apple.LSSharedFileList.RecentDocumentItems", "com.apple.LSSharedFileList.RecentServerItems",
    "com.apple.LSSharedFileList.SessionLoginItems", "com.apple.LSSharedFileList.GlobalLoginItems"};

// MacErrors.h
constexpr std::int32_t param_err = -50;

class SharedFileLists final : public Provider {
public:
    using Provider::Provider;
    Address shared_file_list_class{};

    void install() {
        // LSSharedFileListCreate(allocator, listType, options) returns a +1 list
        // for a known type and NULL otherwise; a snapshot is a +1 empty CFArray.
        shared_file_list_class = objc.find_class("_AlchemyLSSharedFileList");
        if (!shared_file_list_class)
            shared_file_list_class = objc.register_native_class("_AlchemyLSSharedFileList", objc.find_class("NSObject"), 16);
        function(core_services, "_LSSharedFileListCreate", [this](CpuState& s, GuestMemory&) {
            if (!s.x[1]) { s.x[0] = 0; return; }
            auto type = cf.utf8(s.x[1]);
            if (std::find(std::begin(shared_file_list_types), std::end(shared_file_list_types), type) == std::end(shared_file_list_types)) { s.x[0] = 0; return; }
            auto payload = std::make_shared<SharedFileList>();
            payload->type = std::move(type);
            s.x[0] = context.objects().create(16, shared_file_list_class, ObjectKind::instance, std::move(payload));
        });
        function(core_services, "_LSSharedFileListCopySnapshot", [this](CpuState& s, GuestMemory& m) {
            if (s.x[1]) m.write32(s.x[1], 0);
            if (!s.x[0]) { s.x[0] = 0; return; }
            const auto record = context.objects().require(s.x[0]);
            if (!std::dynamic_pointer_cast<SharedFileList>(record->payload))
                throw std::runtime_error("Alchemy CoreServices: object is not an LSSharedFileList");
            s.x[0] = empty_array();
        });
        function(core_services, "_LSSharedFileListItemResolve", [](CpuState& s, GuestMemory& m) {
            if (s.x[2]) m.write64(s.x[2], 0);
            s.x[0] = status(param_err);
        });
        string_symbol(core_services, "_kLSSharedFileListFavoriteItems", "com.apple.LSSharedFileList.FavoriteItems");
    }
};

} // namespace

void install_coreservices_shared_file_lists(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<SharedFileLists>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
