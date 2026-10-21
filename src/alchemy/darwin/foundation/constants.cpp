#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

// Foundation and AppKit constants and functions QLab binds that no provider
// exports (each is added only when missing): NSFileManager attribute keys and
// types, key-value observing change keys, NSUndoManager and other
// notification names, NSURL resource keys, accessibility notification
// names, and the NSRect functions NSUnionRect / NSIntegralRect /
// NSContainsRect / NSEqualRects / NSEqualPoints. Values are the strings
// Foundation and AppKit use.
namespace alchemy::gui::completion_kit {
namespace {
constexpr std::string_view core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

double real(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
void put_real(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
bool empty(double width, double height) { return !(width > 0) || !(height > 0); }
} // namespace

void install_constants(Support& support) {
    const auto fill_string = [&support](std::string_view library, const std::string& name, std::string_view value) {
        const auto symbol = "_" + name;
        if (support.process.resolve(library, symbol)) return;
        support.string_constant(library, symbol, value);
    };
    // Foundation constants whose value is their own name.
    for (const auto* name : {"NSFileType", "NSFileSize", "NSFileModificationDate", "NSFileReferenceCount", "NSFileDeviceIdentifier",
             "NSFileOwnerAccountName", "NSFileGroupOwnerAccountName", "NSFilePosixPermissions", "NSFileSystemNumber",
             "NSFileSystemFileNumber", "NSFileExtensionHidden", "NSFileHFSCreatorCode", "NSFileHFSTypeCode", "NSFileImmutable",
             "NSFileAppendOnly", "NSFileCreationDate", "NSFileOwnerAccountID", "NSFileGroupOwnerAccountID", "NSFileBusy",
             "NSFileProtectionKey", "NSFileTypeDirectory", "NSFileTypeRegular", "NSFileTypeSymbolicLink", "NSFileTypeSocket",
             "NSFileTypeCharacterSpecial", "NSFileTypeBlockSpecial", "NSFileTypeUnknown", "NSFileSystemSize", "NSFileSystemFreeSize",
             "NSFileSystemNodes", "NSFileSystemFreeNodes",
             "NSFileHandleReadToEndOfFileCompletionNotification", "NSFileHandleReadCompletionNotification",
             "NSFileHandleDataAvailableNotification", "NSFileHandleNotificationDataItem", "NSTaskDidTerminateNotification",
             "NSUndoManagerCheckpointNotification", "NSUndoManagerWillUndoChangeNotification", "NSUndoManagerWillRedoChangeNotification",
             "NSUndoManagerDidUndoChangeNotification", "NSUndoManagerDidRedoChangeNotification", "NSUndoManagerDidOpenUndoGroupNotification",
             "NSUndoManagerWillCloseUndoGroupNotification", "NSUndoManagerDidCloseUndoGroupNotification", "NSUndoManagerGroupIsDiscardableKey"})
        fill_string(foundation_library, name, name);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"NSKeyValueChangeKindKey", "kind"}, {"NSKeyValueChangeNewKey", "new"}, {"NSKeyValueChangeOldKey", "old"},
             {"NSKeyValueChangeIndexesKey", "indexes"}, {"NSKeyValueChangeNotificationIsPriorKey", "notificationIsPrior"},
             {"NSKeyedArchiveRootObjectKey", "root"}})
        fill_string(foundation_library, name, value);
    // NSURL resource keys (CoreFoundation's, re-exported by Foundation).
    for (const auto* name : {"NSURLContentTypeKey", "NSURLEffectiveIconKey", "NSURLTotalFileSizeKey", "NSURLVolumeIdentifierKey",
             "NSURLTotalFileAllocatedSizeKey", "NSURLLocalizedNameKey", "NSURLTypeIdentifierKey"}) {
        fill_string(core_foundation_library, name, name);
        fill_string(foundation_library, name, name);
    }
    // AppKit.
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"NSAccessibilityAnnouncementKey", "AXAnnouncementKey"}, {"NSAccessibilityPriorityKey", "AXPriorityKey"},
             {"NSAccessibilityAnnouncementRequestedNotification", "AXAnnouncementRequested"},
             {"NSAccessibilityLayoutChangedNotification", "AXLayoutChanged"}, {"NSAccessibilityRowCollapsedNotification", "AXRowCollapsed"},
             {"NSAccessibilityRowExpandedNotification", "AXRowExpanded"},
             {"NSAccessibilitySelectedRowsChangedNotification", "AXSelectedRowsChanged"},
             {"NSDraggingImageComponentIconKey", "icon"}, {"NSDraggingImageComponentLabelKey", "label"},
             {"NSTextMovementUserInfoKey", "NSTextMovement"}}) fill_string(appkit_library, name, value);
    for (const auto* name : {"NSPopUpButtonCellWillPopUpNotification", "NSPopUpButtonWillPopUpNotification",
             "NSScrollViewWillStartLiveScrollNotification", "NSScrollViewDidLiveScrollNotification", "NSScrollViewDidEndLiveScrollNotification",
             "NSWorkspaceDesktopImageFillColorKey", "NSWorkspaceDesktopImageScalingKey", "NSWorkspaceDesktopImageAllowClippingKey",
             "NSWorkspaceDidMountNotification", "NSWorkspaceDidUnmountNotification", "NSWorkspaceWillUnmountNotification"})
        fill_string(appkit_library, name, name);

    // NSRect functions: rectangles are HFAs of four doubles (d0-d3, d4-d7).
    support.fill_function(foundation_library, "_NSUnionRect", [](CpuState& s, GuestMemory&) {
        const std::array<double, 4> a{real(s, 0), real(s, 1), real(s, 2), real(s, 3)}, b{real(s, 4), real(s, 5), real(s, 6), real(s, 7)};
        std::array<double, 4> result{};
        if (empty(a[2], a[3])) result = empty(b[2], b[3]) ? std::array<double, 4>{} : b;
        else if (empty(b[2], b[3])) result = a;
        else {
            const auto left = std::min(a[0], b[0]), top = std::min(a[1], b[1]);
            result = {left, top, std::max(a[0] + a[2], b[0] + b[2]) - left, std::max(a[1] + a[3], b[1] + b[3]) - top};
        }
        for (unsigned i = 0; i < 4; ++i) put_real(s, i, result[i]);
    });
    support.fill_function(foundation_library, "_NSIntegralRect", [](CpuState& s, GuestMemory&) {
        const double x = real(s, 0), y = real(s, 1), w = real(s, 2), h = real(s, 3);
        if (empty(w, h)) { for (unsigned i = 0; i < 4; ++i) put_real(s, i, 0); return; }
        const auto left = std::floor(x), top = std::floor(y);
        put_real(s, 0, left); put_real(s, 1, top);
        put_real(s, 2, std::ceil(x + w) - left); put_real(s, 3, std::ceil(y + h) - top);
    });
    support.fill_function(foundation_library, "_NSContainsRect", [](CpuState& s, GuestMemory&) {
        const double ax = real(s, 0), ay = real(s, 1), aw = real(s, 2), ah = real(s, 3);
        const double bx = real(s, 4), by = real(s, 5), bw = real(s, 6), bh = real(s, 7);
        s.x[0] = !empty(bw, bh) && ax <= bx && ay <= by && ax + aw >= bx + bw && ay + ah >= by + bh;
    });
    support.fill_function(foundation_library, "_NSEqualRects", [](CpuState& s, GuestMemory&) {
        s.x[0] = real(s, 0) == real(s, 4) && real(s, 1) == real(s, 5) && real(s, 2) == real(s, 6) && real(s, 3) == real(s, 7);
    });
    // NSIsEmptyRect and NSEqualSizes are the headless base's (foundation/geometry.cpp).
    support.fill_function(foundation_library, "_NSEqualPoints", [](CpuState& s, GuestMemory&) { s.x[0] = real(s, 0) == real(s, 2) && real(s, 1) == real(s, 3); });
}

} // namespace alchemy::gui::completion_kit
