// The CoreFoundation, Foundation, CoreServices and AuthenticationServices names Firefox's XUL links
// (cf_extra.cpp, core_foundation.cpp, cf_sets_notifications.cpp, foundation/legacy_classes.cpp,
// coreservices/launch_services.cpp, authenticationservices/install.cpp), and the names VLC's optional plug-ins
// and mpv bind (security/secure_transport.cpp, security/gss.cpp).
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace fill_test;

constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::string_view foundation_lib = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view as_lib = "/System/Library/Frameworks/AuthenticationServices.framework/Versions/A/AuthenticationServices";

const Case foundation_firefox_xul_case("foundation_firefox_xul", [](Fixture& f) {
    // Every name binds.
    for (const auto* name : {"_CFBundleIsArchitectureLoadable", "_CFMessagePortCreateRunLoopSource", "_CFNotificationCenterPostNotification",
                             "_CFPropertyListCreateWithStream", "_CFURLCreateFromFSRef", "_CFURLEnumeratorCreateForDirectoryURL", "_CFURLGetFSRef",
                             "_kCFLocaleCurrentLocaleDidChangeNotification", "_kCFURLIsApplicationKey", "_kCFURLQuarantinePropertiesKey"})
        static_cast<void>(f.symbol(cf_lib, name));
    for (const auto* name : {"_LSCopyApplicationForMIMEType", "_LSGetApplicationForItem", "_LSOpenFromRefSpec"}) static_cast<void>(f.symbol(cs, name));
    for (const auto* name : {"_OBJC_CLASS_$_NSConnection", "_OBJC_CLASS_$_NSSocketPortNameServer", "_OBJC_CLASS_$_NSURLFileTypeMappings",
                             "_OBJC_CLASS_$_NSUserNotificationAction"})
        static_cast<void>(f.symbol(foundation_lib, name));
    for (const auto* name : {"_OBJC_CLASS_$_ASAuthorizationSingleSignOnCredential", "_OBJC_CLASS_$_ASWebAuthenticationSessionRequest"})
        static_cast<void>(f.symbol(as_lib, name));
    require(f.string_constant(cf_lib, "_kCFLocaleCurrentLocaleDidChangeNotification") == "kCFLocaleCurrentLocaleDidChangeNotification",
            "kCFLocaleCurrentLocaleDidChangeNotification");
    require(f.string_constant(cf_lib, "_kCFURLIsApplicationKey") == "NSURLIsApplicationKey", "kCFURLIsApplicationKey");

    // CFBundleIsArchitectureLoadable: arm64 only.
    require(f.call(cf_lib, "_CFBundleIsArchitectureLoadable", {0x0100000C}) == 1 && f.call(cf_lib, "_CFBundleIsArchitectureLoadable", {0x01000007}) == 0,
            "only arm64 loads");

    // A directory to work in.
    const auto host = std::filesystem::temp_directory_path() /
                      ("alchemy_firefox_xul_case_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(host);
    std::filesystem::create_directories(host / "Example.app");
    { std::ofstream(host / "notes.txt") << "twelve bytes"; }
    { std::ofstream(host / "list.plist") << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict><key>answer</key><integer>42</integer></dict></plist>\n"; }
    const auto root = alchemy::guest_path_string(host);

    // FSRefs from and to URLs.
    const auto file = f.cf.file_url(root + "/notes.txt", false);
    const auto ref = f.block(80);
    require(f.call(cf_lib, "_CFURLGetFSRef", {file, ref}) == 1, "CFURLGetFSRef");
    const auto back = f.call(cf_lib, "_CFURLCreateFromFSRef", {0, ref});
    require(back && f.cf.url_path(back).ends_with("/notes.txt"), "CFURLCreateFromFSRef");
    require(f.call(cf_lib, "_CFURLGetFSRef", {f.cf.file_url(root + "/missing.txt", false), f.block(80)}) == 0, "no FSRef for a missing item");

    // Resource keys.
    const auto value = f.block(8, 0xff);
    const auto app = f.cf.file_url(root + "/Example.app", true);
    require(f.call(cf_lib, "_CFURLCopyResourcePropertyForKey", {app, f.engine.read64(f.symbol(cf_lib, "_kCFURLIsApplicationKey")), value, 0}) == 1 &&
                f.engine.read64(value) == f.engine.read64(f.symbol(cf_lib, "_kCFBooleanTrue")),
            "an .app directory is an application");
    require(f.call(cf_lib, "_CFURLCopyResourcePropertyForKey", {file, f.engine.read64(f.symbol(cf_lib, "_kCFURLQuarantinePropertiesKey")), value, 0}) == 1 &&
                f.engine.read64(value) == 0,
            "no quarantine properties");

    // The directory enumerator.
    const auto directory = f.cf.file_url(root, true);
    const auto enumerator = f.call(cf_lib, "_CFURLEnumeratorCreateForDirectoryURL", {0, directory, 0, 0});
    std::set<std::string> names;
    const auto next = f.block(8);
    while (f.call(cf_lib, "_CFURLEnumeratorGetNextURL", {enumerator, next, 0}) == 1) {
        const auto path = f.cf.url_path(f.engine.read64(next));
        names.insert(path.substr(path.find_last_of('/', path.size() - 2) + 1));
    }
    require(names.contains("notes.txt") && names.contains("list.plist") && names.size() == 3, "the directory's three items");
    bool stopped = false;
    try { static_cast<void>(f.call(cf_lib, "_CFURLEnumeratorCreateForDirectoryURL", {0, directory, 1, 0})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "other enumeration options stop explicitly");
    const auto unreadable = f.call(cf_lib, "_CFURLEnumeratorCreateForDirectoryURL", {0, f.cf.file_url(root + "/missing", true), 0, 0});
    require(f.call(cf_lib, "_CFURLEnumeratorGetNextURL", {unreadable, next, 0}) == 3, "a missing directory reports kCFURLEnumeratorError");

    // A property list from a stream.
    const auto stream = f.call(cf_lib, "_CFReadStreamCreateWithFile", {0, f.cf.file_url(root + "/list.plist", false)});
    require(stream && f.call(cf_lib, "_CFReadStreamOpen", {stream}) == 1, "open the stream");
    const auto list = f.call(cf_lib, "_CFPropertyListCreateWithStream", {0, stream, 0, 0, 0, 0});
    require(list && f.send(f.send(list, "objectForKey:", {f.cf.string("answer")}), "integerValue") == 42, "CFPropertyListCreateWithStream");

    // Posting to the Darwin notify center reaches this process's observers of that name.
    int heard = 0;
    const auto callback = f.process.install_function("_firefox_xul_test_notification", [&heard](transmute::CpuState&, transmute::GuestMemory&) { ++heard; }, "alchemy.test");
    const auto darwin = f.call(cf_lib, "_CFNotificationCenterGetDarwinNotifyCenter");
    const auto name = f.cf.string("com.example.firefox-xul-test");
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterAddObserver", {darwin, 0x1234, callback, name, 0, 0}));
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterPostNotification", {darwin, name, 0, 0, 1}));
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterPostNotification", {darwin, f.cf.string("com.example.other"), 0, 0, 1}));
    require(heard == 1, "one matching Darwin notification");
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterRemoveEveryObserver", {darwin, 0x1234}));
    static_cast<void>(f.call(cf_lib, "_CFNotificationCenterPostNotification", {darwin, name, 0, 0, 1}));
    require(heard == 1, "no delivery after removal");

    // A message port's run loop source.
    const auto port = f.call(cf_lib, "_CFMessagePortCreateLocal", {0, f.cf.string("com.example.firefox-xul-port"), callback, 0, 0});
    const auto source = f.call(cf_lib, "_CFMessagePortCreateRunLoopSource", {0, port, 0});
    require(port && source, "CFMessagePortCreateRunLoopSource");

    // UTType tags of every class (NSURLFileTypeMappings' source).
    const auto mime_class = f.engine.read64(f.symbol(cs, "_kUTTagClassMIMEType"));
    require(f.cf.utf8(f.call(cs, "_UTTypeCopyPreferredTagWithClass", {f.cf.string("public.png"), mime_class})) == "image/png" &&
                f.cf.utf8(f.call(cs, "_UTTypeCreatePreferredIdentifierForTag", {mime_class, f.cf.string("image/png"), 0})) == "public.png",
            "MIME type tags");

    // Foundation classes.
    const auto mappings = f.send(f.cls("NSURLFileTypeMappings"), "sharedMappings");
    require(f.cf.utf8(f.send(mappings, "MIMETypeForExtension:", {f.cf.string("png")})) == "image/png", "MIMETypeForExtension:");
    require(f.cf.utf8(f.send(mappings, "preferredExtensionForMIMEType:", {f.cf.string("image/png")})) == "png", "preferredExtensionForMIMEType:");
    require(f.send(f.cls("NSConnection"), "connectionWithRegisteredName:host:", {f.cf.string("com.example.server"), 0}) == 0, "no vended connection");
    const auto name_server = f.send(f.cls("NSSocketPortNameServer"), "sharedInstance");
    require(name_server && f.send(name_server, "portForName:", {f.cf.string("x")}) == 0 &&
                (f.send(name_server, "registerPort:name:", {0, f.cf.string("x")}) & 0xff) == 0,
            "no reachable port name server");
    const auto action = f.send(f.cls("NSUserNotificationAction"), "actionWithIdentifier:title:", {f.cf.string("reply"), f.cf.string("Reply")});
    require(f.cf.utf8(f.send(action, "identifier")) == "reply" && f.cf.utf8(f.send(action, "title")) == "Reply", "NSUserNotificationAction");

    // Launch Services over FSRefs and MIME types (no application here handles PNG files).
    const auto app_url = f.block(8, 0xff);
    require(f.status(cs, "_LSCopyApplicationForMIMEType", {f.cf.string("image/png"), 0xffffffff, app_url}) == -10814 && f.engine.read64(app_url) == 0,
            "LSCopyApplicationForMIMEType");
    require(f.status(cs, "_LSGetApplicationForItem", {ref, 0xffffffff, 0, app_url}) == -10814, "LSGetApplicationForItem");
    require(f.status(cs, "_LSOpenFromRefSpec", {0, 0}) == -50, "LSOpenFromRefSpec without a spec");
    const auto spec = f.block(48);
    f.engine.write64(spec, ref);   // an application FSRef: launching a guest application is refused
    require(f.status(cs, "_LSOpenFromRefSpec", {spec, 0}) == -10822, "LSOpenFromRefSpec of an application");

    std::error_code error;
    std::filesystem::remove_all(host, error);
});

// VLC's optional plug-ins and mpv's Kerberos calls.
const Case foundation_vlc_mpv_case("foundation_vlc_mpv", [](Fixture& f) {
    constexpr std::string_view security = "/System/Library/Frameworks/Security.framework/Versions/A/Security";
    constexpr std::string_view kerberos = "/System/Library/Frameworks/Kerberos.framework/Versions/A/Kerberos";
    // CFStringTokenizerCopyBestStringLanguage: only a writing system that names a language.
    const auto japanese = f.cf.string("\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf");   // kana
    const auto best = f.call(cf_lib, "_CFStringTokenizerCopyBestStringLanguage", {japanese, 0, 5});
    require(best && f.cf.utf8(best) == "ja", "kana is Japanese");
    require(f.call(cf_lib, "_CFStringTokenizerCopyBestStringLanguage", {f.cf.string("hello"), 0, 5}) == 0, "Latin text alone names no language");
    require(f.string_constant(foundation_lib, "_NSConnectionDidDieNotification") == "NSConnectionDidDieNotification", "NSConnectionDidDieNotification");
    // SSLCopyPeerCertificates before any handshake: NULL.
    const auto context = f.call(security, "_SSLCreateContext", {0, 1, 0});
    const auto certificates = f.block(8, 0xff);
    require(context && f.status(security, "_SSLCopyPeerCertificates", {context, certificates}) == 0 && f.engine.read64(certificates) == 0,
            "no peer certificates yet");
    // GSS OID sets.
    const auto minor = f.block(4), set = f.block(8), present = f.block(4);
    const auto oid = f.block(16), elements = f.block(9);
    const std::vector<std::uint8_t> krb5{0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x02};   // 1.2.840.113554.1.2.2 (RFC 4121)
    f.engine.write_bytes(elements, krb5);
    f.engine.write32(oid, 9);
    f.engine.write64(oid + 8, elements);
    require(f.status(kerberos, "_gss_create_empty_oid_set", {minor, set}) == 0 && f.engine.read64(f.engine.read64(set)) == 0, "an empty OID set");
    require(f.status(kerberos, "_gss_test_oid_set_member", {minor, oid, f.engine.read64(set), present}) == 0 && f.engine.read32(present) == 0,
            "not a member yet");
    require(f.status(kerberos, "_gss_add_oid_set_member", {minor, oid, set}) == 0 && f.status(kerberos, "_gss_add_oid_set_member", {minor, oid, set}) == 0 &&
                f.engine.read64(f.engine.read64(set)) == 1,
            "added once");
    require(f.status(kerberos, "_gss_test_oid_set_member", {minor, oid, f.engine.read64(set), present}) == 0 && f.engine.read32(present) == 1,
            "a member now");
    require(f.status(kerberos, "_gss_release_oid_set", {minor, set}) == 0 && f.engine.read64(set) == 0, "released");
    const auto handle = f.block(8, 0xff);
    require(f.status(kerberos, "_gss_release_oid", {minor, handle}) == 0 && f.engine.read64(handle) == 0, "gss_release_oid clears the handle");
    require(static_cast<std::uint32_t>(f.call(kerberos, "_gss_inquire_cred_by_mech", {minor, 0, oid, 0, 0, 0, 0})) == (7u << 16),
            "no credential (GSS_S_NO_CRED)");
});

} // namespace
