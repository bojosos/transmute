// URLs over the full provider set (fill_fixture.hpp): NSURLComponents
// (url_components.cpp), the CFURL additions (corefoundation/cf_urls.cpp) and
// the NSURL file-URL additions (url_extra.cpp). Range results in x1 are not
// visible to this engine: only their locations are checked. File URL paths
// are stored host-mapped (/tmp is the host temp directory), so the paths here
// are under /Volumes/C, which maps to itself.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <vector>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr Address not_found = 0x7fffffffffffffffULL;
bool yes(Address value) { return (value & 0xff) != 0; }

std::string text(Fixture& f, Address string) { return string ? f.cf.utf8(string) : std::string("(nil)"); }
Address str(Fixture& f, std::string_view value) { return f.cf.string(value); }

void components(Fixture& f) {
    const auto cls = f.cls("NSURLComponents");
    const auto parts = f.send(cls, "componentsWithString:", {str(f, "https://us%40r:pa%3Ass@example.com:8443/a%20b/c?x=1&y=caf%C3%A9&flag#frag%21")});
    require(parts != 0, "parse");
    require(text(f, f.send(parts, "scheme")) == "https" && text(f, f.send(parts, "user")) == "us@r" &&
                text(f, f.send(parts, "password")) == "pa:ss" && text(f, f.send(parts, "host")) == "example.com", "scheme, user, password, host");
    require(f.send(f.send(parts, "port"), "integerValue") == 8443, "port");
    require(text(f, f.send(parts, "path")) == "/a b/c" && text(f, f.send(parts, "percentEncodedPath")) == "/a%20b/c", "decoded and encoded path");
    require(text(f, f.send(parts, "query")) == "x=1&y=caf\xc3\xa9&flag" && text(f, f.send(parts, "fragment")) == "frag!", "query and fragment");
    const auto items = f.send(parts, "queryItems");
    require(f.send(items, "count") == 3, "three query items");
    const auto second = f.send(items, "objectAtIndex:", {1});
    require(text(f, f.send(second, "name")) == "y" && text(f, f.send(second, "value")) == "caf\xc3\xa9", "a decoded query item");
    require(f.send(f.send(items, "objectAtIndex:", {2}), "value") == 0, "a name without a value");
    require(f.send(parts, "rangeOfHost") == 23 && f.send(parts, "rangeOfScheme") == 0, "component ranges");
    // Building one.
    const auto built = f.send(f.send(cls, "alloc"), "init");
    static_cast<void>(f.send(built, "setScheme:", {str(f, "http")}));
    static_cast<void>(f.send(built, "setHost:", {str(f, "localhost")}));
    static_cast<void>(f.send(built, "setPath:", {str(f, "/a b")}));
    const auto item_class = f.cls("NSURLQueryItem");
    const auto list = f.call(cf, "_CFArrayCreateMutable", {0, 0, f.symbol(cf, "_kCFTypeArrayCallBacks")});
    static_cast<void>(f.call(cf, "_CFArrayAppendValue", {list, f.send(item_class, "queryItemWithName:value:", {str(f, "q"), str(f, "a&b c")})}));
    static_cast<void>(f.send(built, "setQueryItems:", {list}));
    static_cast<void>(f.send(built, "setFragment:", {str(f, "top")}));
    require(text(f, f.send(built, "string")) == "http://localhost/a%20b?q=a%26b%20c#top", "-string composes and encodes");
    const auto url = f.send(built, "URL");
    require(url && text(f, f.send(url, "host")) == "localhost", "-URL is a real NSURL");
    require(f.send(built, "rangeOfPort") == not_found, "an absent port has no range");
    static_cast<void>(f.send(built, "setPath:", {str(f, "relative")}));
    require(f.send(built, "string") == 0 && f.send(built, "URL") == 0, "a relative path after an authority is invalid");
    require(f.send(cls, "componentsWithString:", {str(f, "http://exa mple.com/")}) == 0, "an invalid string gives nil");
    const auto copy = f.send(parts, "copy");
    require(yes(f.send(copy, "isEqual:", {parts})), "copies are equal");
    bool refused = false;
    try { static_cast<void>(f.send(built, "setPercentEncodedPath:", {str(f, "/a b")})); } catch (const std::exception&) { refused = true; }
    require(refused, "an invalid percent-encoded path raises");
    const auto from_url = f.send(cls, "componentsWithURL:resolvingAgainstBaseURL:",
        {f.send(f.cls("NSURL"), "URLWithString:", {str(f, "ftp://host/file.txt")}), 1});
    require(text(f, f.send(from_url, "path")) == "/file.txt", "componentsWithURL:");
}

void cf_urls(Fixture& f) {
    const auto file = f.send(f.cls("NSURL"), "fileURLWithPath:isDirectory:", {str(f, "/Volumes/C/alchemy-url-test/folder/report.txt"), 0});
    require(text(f, f.call(cf, "_CFURLCopyLastPathComponent", {file})) == "report.txt", "CFURLCopyLastPathComponent");
    require(text(f, f.call(cf, "_CFURLCopyPathExtension", {file})) == "txt", "CFURLCopyPathExtension");
    const auto bare = f.send(f.cls("NSURL"), "fileURLWithPath:isDirectory:", {str(f, "/Volumes/C/alchemy-url-test/folder"), 1});
    require(f.call(cf, "_CFURLCopyPathExtension", {bare}) == 0, "no extension: NULL");
    const auto appended = f.call(cf, "_CFURLCreateCopyAppendingPathComponent", {0, bare, str(f, "x.png"), 0});
    const auto appended_path = text(f, f.send(appended, "path"));
    require(appended_path == "/Volumes/C/alchemy-url-test/folder/x.png", "CFURLCreateCopyAppendingPathComponent: " + appended_path);
    require(text(f, f.send(f.call(cf, "_CFURLCreateCopyDeletingPathExtension", {0, appended}), "path")) == "/Volumes/C/alchemy-url-test/folder/x",
            "CFURLCreateCopyDeletingPathExtension");
    require(text(f, f.send(f.call(cf, "_CFURLCreateCopyAppendingPathExtension", {0, bare, str(f, "app")}), "path")) == "/Volumes/C/alchemy-url-test/folder.app",
            "CFURLCreateCopyAppendingPathExtension");
    const auto absolute = f.block(1);
    require(text(f, f.call(cf, "_CFURLCopyStrictPath", {file, absolute})) == "Volumes/C/alchemy-url-test/folder/report.txt" && f.memory().read8(absolute) == 1,
            "CFURLCopyStrictPath");
    const auto relative = f.call(cf, "_CFURLCreateWithFileSystemPathRelativeToBase", {0, str(f, "inner/a.txt"), 0, 0, bare});
    require(text(f, f.send(relative, "path")) == "/Volumes/C/alchemy-url-test/folder/inner/a.txt", "a path relative to a base");
    const auto web = f.send(f.cls("NSURL"), "URLWithString:", {str(f, "http://user@host.example:81/p;param?q=1#f")});
    const auto length = f.call(cf, "_CFURLGetBytes", {web, 0, 0});
    const auto buffer = f.block(length + 1);
    require(length == 41 && f.call(cf, "_CFURLGetBytes", {web, buffer, length}) == length && f.call(cf, "_CFURLGetBytes", {web, buffer, 3}) == static_cast<Address>(-1),
            "CFURLGetBytes");
    require(f.call(cf, "_CFURLGetByteRangeForComponent", {web, 8, 0}) == 12 && f.call(cf, "_CFURLGetByteRangeForComponent", {web, 9, 0}) == 25 &&
                f.call(cf, "_CFURLGetByteRangeForComponent", {web, 10, 0}) == 30 && f.call(cf, "_CFURLGetByteRangeForComponent", {web, 11, 0}) == 36 &&
                f.call(cf, "_CFURLGetByteRangeForComponent", {web, 12, 0}) == 40, "byte ranges of host, port, parameter, query, fragment");
    require(text(f, f.call(cf, "_CFURLCopyParameterString", {web, str(f, "")})) == "param", "CFURLCopyParameterString");
    // Percent escapes: CF's legal characters; "%" and "#" are escaped.
    require(text(f, f.call(cf, "_CFURLCreateStringByAddingPercentEscapes", {0, str(f, "a b%#/c\xc3\xa9"), 0, str(f, "/"), 0x08000100})) ==
                "a%20b%25%23%2Fc%C3%A9", "CFURLCreateStringByAddingPercentEscapes");
    require(text(f, f.call(cf, "_CFURLCreateStringByAddingPercentEscapes", {0, str(f, "a b"), str(f, " "), 0, 0x08000100})) == "a b",
            "characters left unescaped");
    require(text(f, f.call(cf, "_CFURLCreateStringByReplacingPercentEscapes", {0, str(f, "a%20b%2Fc%C3%A9"), str(f, "/")})) == "a b%2Fc\xc3\xa9",
            "CFURLCreateStringByReplacingPercentEscapes");
    require(text(f, f.call(cf, "_CFURLCreateStringByReplacingPercentEscapes", {0, str(f, "a%20b"), 0})) == "a%20b", "NULL leaves every escape");
    require(f.call(cf, "_CFURLCreateStringByReplacingPercentEscapes", {0, str(f, "%FF"), str(f, "")}) == 0, "invalid UTF-8 gives NULL");
    require(text(f, f.call(cf, "_CFURLCreateStringByReplacingPercentEscapesUsingEncoding", {0, str(f, "caf%E9"), str(f, ""), 0x0201})) == "caf\xc3\xa9",
            "...UsingEncoding (ISO Latin 1)");
    require(f.string_constant(cf, "_kCFURLCreationDateKey") == "NSURLCreationDateKey", "kCFURLCreationDateKey");
    require(f.call(cf, "_CFURLCreateFilePathURL", {0, file, 0}) != 0 && yes(f.call(cf, "_CFURLStartAccessingSecurityScopedResource", {file})),
            "file path URL and security scope");
}

void ns_file_urls(Fixture& f) {
    const auto url_class = f.cls("NSURL");
    const auto file = f.send(url_class, "fileURLWithPath:isDirectory:", {str(f, "/Volumes/C/alchemy-url-test/a b.txt"), 0});
    require(f.send(file, "host") == 0 && f.send(file, "query") == 0 && f.send(file, "fragment") == 0 && f.send(file, "port") == 0 &&
                f.send(file, "baseURL") == 0, "a file URL has no authority, query or fragment");
    require(text(f, f.send(file, "resourceSpecifier")) == "///Volumes/C/alchemy-url-test/a%20b.txt", "resourceSpecifier");
    const auto data = f.send(file, "dataRepresentation");
    require(text(f, f.send(f.send(url_class, "URLWithDataRepresentation:relativeToURL:", {data, 0}), "path")) == "/Volumes/C/alchemy-url-test/a b.txt",
            "dataRepresentation round trip");
    const auto list = f.call(cf, "_CFArrayCreateMutable", {0, 0, f.symbol(cf, "_kCFTypeArrayCallBacks")});
    for (const auto* part : {"/", "Volumes", "C", "lib"}) static_cast<void>(f.call(cf, "_CFArrayAppendValue", {list, str(f, part)}));
    require(text(f, f.send(f.send(url_class, "fileURLWithPathComponents:", {list}), "path")) == "/Volumes/C/lib", "+fileURLWithPathComponents:");
    const auto base = f.send(url_class, "fileURLWithPath:isDirectory:", {str(f, "/Volumes/C/var"), 1});
    require(text(f, f.send(f.send(url_class, "fileURLWithPath:relativeToURL:", {str(f, "log/x"), base}), "path")) == "/Volumes/C/var/log/x",
            "+fileURLWithPath:relativeToURL:");
    require(!yes(f.send(file, "isFileReferenceURL")), "no file reference URLs");
}

} // namespace

int main() {
    return run("foundation urls", {{"NSURLComponents", components}, {"CFURL", cf_urls}, {"NSURL file URLs", ns_file_urls}});
}
