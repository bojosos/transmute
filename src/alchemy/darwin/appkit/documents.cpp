#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// NSDocument / NSDocumentController (QLab's workspaces): the document
// architecture over the application's CFBundleDocumentTypes.
//   NSDocumentController  the shared controller is the first one initialized
//                         (an application's subclass made in its main nib
//                         wins), else +sharedDocumentController makes one.
//                         Types are the CFBundleDocumentTypes entries: a
//                         type name is the entry's first LSItemContentTypes
//                         UTI (else its CFBundleTypeName); the default type
//                         is the first Editor entry with an NSDocumentClass;
//                         URLs map to types by filename extension
//                         (UTExported/ImportedTypeDeclarations and
//                         CFBundleTypeExtensions). Untitled documents and
//                         documents read from a URL are made, added and shown.
//   NSDocument            file URL and type, change count, display name,
//                         window controllers (-makeWindowControllers loads
//                         -windowNibName with the document as File's Owner;
//                         the nib's `window` outlet reaches the loading
//                         controller through -setWindow:), -showWindows,
//                         -close, an NSUndoManager on demand. Reading goes
//                         -readFromURL:ofType:error: -> -readFromData:ofType:error:
//                         for files; file packages (directories) fail
//                         explicitly. Saving: -saveDocument:,
//                         -saveDocumentAs: / To: (the save panel),
//                         -saveDocumentWithDelegate:didSaveSelector:contextInfo:,
//                         -saveToURL:ofType:forSaveOperation:delegate:... and
//                         ...completionHandler: go -writeSafelyToURL:... ->
//                         -writeToURL:ofType:forSaveOperation:originalContentsURL:error:
//                         -> -writeToURL:ofType:error: -> -dataOfType:error:
//                         (or an overridden -fileWrapperOfType:error:), written
//                         to a temporary file and moved over the target.
//                         Drafts, the autosaved contents URL and the backup URL
//                         (none: backups are not kept) are properties.
//                         Recent documents: resources.cpp.
// Open panels: -openDocument: / -URLsFromRunningOpenPanel (panels.cpp).
// Autosaving (in place or elsewhere) is not provided.
namespace alchemy::gui::appkit {
namespace {

struct DocumentState final : ObjectPayload {
    Address file_url{}, file_type{}, display_name{}, undo_manager{};   // +1
    Address autosaved_url{};                                           // +1
    bool draft{};
    std::vector<Address> window_controllers;                           // +1
    std::int64_t change_count{};
    bool has_undo_manager{true}, closing{};
};
struct DocumentControllerState final : ObjectPayload {
    std::vector<Address> documents;                                    // +1
    double autosaving_delay{};
};

struct DocumentType {
    std::string name;            // the first UTI, else CFBundleTypeName
    std::vector<std::string> utis, extensions;
    std::string role, document_class, display_name;
};

} // namespace

void AppKit::install_documents() {
    const auto document_class = declare_class("NSDocument", "NSObject");
    const auto dc = document_controller;
    const auto ds = [this](Address value) { return objc.host_state<DocumentState>(value); };
    const auto dcs = [this](Address value) { return objc.host_state<DocumentControllerState>(value); };

    // ---- the application's document types (Info.plist) ----
    const auto info_value = [this](Address dictionary, std::string_view key) { return send_id(dictionary, "objectForKey:", {auto_string(key)}); };
    const auto strings_of = [this](Address array) {
        std::vector<std::string> values;
        if (array && cf.kind(array) == ObjectKind::array)
            for (const auto value : cf.array_values(array)) if (value && cf.kind(value) == ObjectKind::string) values.push_back(text(value));
        return values;
    };
    const auto document_types = [this, info_value, strings_of]() {
        std::vector<DocumentType> types;
        const auto bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
        if (!bundle) return types;
        const auto entries = send_id(bundle, "objectForInfoDictionaryKey:", {auto_string("CFBundleDocumentTypes")});
        if (!entries || cf.kind(entries) != ObjectKind::array) return types;
        // Filename extensions declared for UTIs.
        std::vector<std::pair<std::string, std::string>> uti_extensions;
        for (const auto* key : {"UTExportedTypeDeclarations", "UTImportedTypeDeclarations"}) {
            const auto declarations = send_id(bundle, "objectForInfoDictionaryKey:", {auto_string(key)});
            if (!declarations || cf.kind(declarations) != ObjectKind::array) continue;
            for (const auto declaration : cf.array_values(declarations)) {
                const auto identifier = info_value(declaration, "UTTypeIdentifier");
                const auto tags = info_value(declaration, "UTTypeTagSpecification");
                if (!identifier || !tags) continue;
                auto extensions = info_value(tags, "public.filename-extension");
                std::vector<std::string> list = cf.kind(extensions) == ObjectKind::string ? std::vector<std::string>{text(extensions)} : strings_of(extensions);
                for (const auto& extension : list) uti_extensions.emplace_back(text(identifier), extension);
            }
        }
        for (const auto entry : cf.array_values(entries)) {
            if (!entry || cf.kind(entry) != ObjectKind::dictionary) continue;
            DocumentType type;
            type.utis = strings_of(info_value(entry, "LSItemContentTypes"));
            type.extensions = strings_of(info_value(entry, "CFBundleTypeExtensions"));
            for (const auto& uti : type.utis)
                for (const auto& [identifier, extension] : uti_extensions) if (identifier == uti) type.extensions.push_back(extension);
            if (const auto value = info_value(entry, "CFBundleTypeName")) type.display_name = text(value);
            if (const auto value = info_value(entry, "CFBundleTypeRole")) type.role = text(value);
            if (const auto value = info_value(entry, "NSDocumentClass")) type.document_class = text(value);
            type.name = type.utis.empty() ? type.display_name : type.utis.front();
            types.push_back(std::move(type));
        }
        return types;
    };
    const auto find_type = [document_types](const std::string& name) -> std::optional<DocumentType> {
        for (auto& type : document_types())
            if (type.name == name || type.display_name == name || std::find(type.utis.begin(), type.utis.end(), name) != type.utis.end()) return type;
        return std::nullopt;
    };
    const auto cocoa_error = [this](std::int64_t code, const std::string& description) {
        const auto error_class = objc.find_class("NSError");
        if (!error_class) return Address{};
        std::vector<std::pair<Address, Address>> entries{{auto_string("NSLocalizedDescription"), auto_string(description)}};
        const auto info = autorelease(cf.dictionary(entries));
        CallRegs registers;
        registers.x[2] = auto_string("NSCocoaErrorDomain");
        registers.x[3] = static_cast<Address>(code);
        registers.x[4] = info;
        return send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    };
    const auto set_error = [this](Address out, Address error) { if (out) memory().write64(out, error); };

    // ---- NSDocumentController ----
    const auto shared = [this]() {
        { std::lock_guard lock(mutex); if (shared_document_controller) return shared_document_controller; }
        const auto made = alloc_init(document_controller);   // -init registers it
        std::lock_guard lock(mutex);
        if (!shared_document_controller) shared_document_controller = made;
        return shared_document_controller;
    };
    class_method(dc, "sharedDocumentController", [shared](CpuState& s, GuestMemory&) { s.x[0] = shared(); });
    method(dc, "init", [this, dc, dcs](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], dc, sel("init")).x0;
        s.x[0] = self;
        if (!self) return;
        static_cast<void>(dcs(self));
        std::lock_guard lock(mutex);
        if (!shared_document_controller) shared_document_controller = retain(self);   // kept for the process
    });
    method(dc, "initWithCoder:", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "init").x0; }, "@@:@");
    method(dc, "documents", [this, dcs](CpuState& s, GuestMemory&) { s.x[0] = auto_array(dcs(s.x[0])->documents); });
    method(dc, "addDocument:", [this, dcs](CpuState& s, GuestMemory&) {
        auto& list = dcs(s.x[0])->documents;
        if (s.x[2] && std::find(list.begin(), list.end(), s.x[2]) == list.end()) list.push_back(retain(s.x[2]));
    }, "v@:@");
    method(dc, "removeDocument:", [this, dcs](CpuState& s, GuestMemory&) {
        auto& list = dcs(s.x[0])->documents;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        release(s.x[2]);
    }, "v@:@");
    method(dc, "documentForWindow:", [this, dcs](CpuState& s, GuestMemory&) {
        s.x[0] = 0;
        if (!s.x[2]) return;
        const auto controller = window_state(s.x[2])->window_controller;
        const auto document = controller ? send(controller, "document").x0 : 0;
        const auto& list = dcs(s.x[0])->documents;
        if (document && std::find(list.begin(), list.end(), document) != list.end()) s.x[0] = document;
    });
    // The document of the main window, else of the key window.
    method(dc, "currentDocument", [this](CpuState& s, GuestMemory&) {
        Address result{};
        std::array<Address, 2> windows{};
        { std::lock_guard lock(mutex); windows = {main_window, key_window}; }
        for (const auto target : windows) {
            if (!target || result) continue;
            result = send_id(s.x[0], "documentForWindow:", {target});
        }
        s.x[0] = result;
    });
    method(dc, "documentForURL:", [this, dcs](CpuState& s, GuestMemory&) {
        s.x[0] = 0;
        if (!s.x[2]) return;
        const auto wanted = cf.url_path(s.x[2]);
        for (const auto document : dcs(s.x[0])->documents) {
            const auto url = send(document, "fileURL").x0;
            if (url && cf.url_path(url) == wanted) { s.x[0] = document; return; }
        }
    });
    method(dc, "hasEditedDocuments", [this, dcs](CpuState& s, GuestMemory&) {
        bool edited = false;
        for (const auto document : dcs(s.x[0])->documents) edited = edited || send_bool(document, "isDocumentEdited");
        return_bool(s, edited);
    }, "B@:");
    method(dc, "documentClassNames", [this, document_types](CpuState& s, GuestMemory&) {
        std::vector<std::string> names;
        for (const auto& type : document_types())
            if (!type.document_class.empty() && std::find(names.begin(), names.end(), type.document_class) == names.end()) names.push_back(type.document_class);
        std::vector<Address> values;
        for (const auto& name : names) values.push_back(auto_string(name));
        s.x[0] = auto_array(values);
    });
    method(dc, "defaultType", [this, document_types](CpuState& s, GuestMemory&) {
        for (const auto& type : document_types())
            if (type.role == "Editor" && !type.document_class.empty()) { s.x[0] = auto_string(type.name); return; }
        s.x[0] = 0;
    });
    method(dc, "documentClassForType:", [this, find_type](CpuState& s, GuestMemory&) {
        const auto type = s.x[2] ? find_type(text(s.x[2])) : std::nullopt;
        s.x[0] = type && !type->document_class.empty() ? objc.find_class(type->document_class) : 0;
    });
    method(dc, "displayNameForType:", [this, find_type](CpuState& s, GuestMemory&) {
        const auto type = s.x[2] ? find_type(text(s.x[2])) : std::nullopt;
        s.x[0] = type && !type->display_name.empty() ? auto_string(type->display_name) : s.x[2];
    });
    method(dc, "typeForContentsOfURL:error:", [this, document_types, cocoa_error, set_error](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? cf.url_path(s.x[2]) : std::string();
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of('/');
        std::string extension = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : path.substr(dot + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& type : document_types())
            for (auto known : type.extensions) {
                std::transform(known.begin(), known.end(), known.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!extension.empty() && known == extension) { s.x[0] = auto_string(type.name); return; }
            }
        set_error(s.x[3], cocoa_error(256, "The document type of " + path + " is not one the application declares."));
        s.x[0] = 0;
    }, "@@:@^@");
    method(dc, "makeUntitledDocumentOfType:error:", [this, cocoa_error, set_error](CpuState& s, GuestMemory&) {
        const auto cls = send_id(s.x[0], "documentClassForType:", {s.x[2]});
        if (!cls) { set_error(s.x[3], cocoa_error(256, "No document class for type " + text(s.x[2]) + ".")); s.x[0] = 0; return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        s.x[0] = autorelease(send(send(cls, "alloc").x0, "initWithType:error:", registers).x0);
    }, "@@:@^@");
    method(dc, "openUntitledDocumentAndDisplay:error:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto display = bool_arg(s, 2);
        const auto type = send(self, "defaultType").x0;
        CallRegs registers;
        registers.x[2] = type;
        registers.x[3] = s.x[3];
        const auto document = type ? send(self, "makeUntitledDocumentOfType:error:", registers).x0 : 0;
        if (!document) { s.x[0] = 0; return; }
        send_id(self, "addDocument:", {document});
        if (display) {
            static_cast<void>(send(document, "makeWindowControllers"));
            static_cast<void>(send(document, "showWindows"));
        }
        s.x[0] = document;
    }, "@@:B^@");
    method(dc, "newDocument:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = 1;
        registers.x[3] = 0;
        static_cast<void>(send(s.x[0], "openUntitledDocumentAndDisplay:error:", registers));
    }, "v@:@");
    method(dc, "makeDocumentWithContentsOfURL:ofType:error:", [this, cocoa_error, set_error](CpuState& s, GuestMemory&) {
        const auto cls = send_id(s.x[0], "documentClassForType:", {s.x[3]});
        if (!cls) { set_error(s.x[4], cocoa_error(256, "No document class for type " + text(s.x[3]) + ".")); s.x[0] = 0; return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        registers.x[4] = s.x[4];
        s.x[0] = autorelease(send(send(cls, "alloc").x0, "initWithContentsOfURL:ofType:error:", registers).x0);
    }, "@@:@@^@");
    // Opens (or finds) the document and calls the handler at once with
    // (document, documentWasAlreadyOpen, error).
    method(dc, "openDocumentWithContentsOfURL:display:completionHandler:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], url = s.x[2], handler = s.x[4];
        const auto display = bool_arg(s, 3);
        Address error{};
        const auto error_slot = process.allocate_guest(8);
        Address document = send_id(self, "documentForURL:", {url});
        const bool already = document != 0;
        try {
            memory().write64(error_slot, 0);
            if (!document) {
                CallRegs type_call;
                type_call.x[2] = url;
                type_call.x[3] = error_slot;
                const auto type = send(self, "typeForContentsOfURL:error:", type_call).x0;
                if (type) {
                    CallRegs make;
                    make.x[2] = url;
                    make.x[3] = type;
                    make.x[4] = error_slot;
                    document = send(self, "makeDocumentWithContentsOfURL:ofType:error:", make).x0;
                }
                if (document) send_id(self, "addDocument:", {document});
            }
            error = memory().read64(error_slot);
        } catch (...) { process.release_guest(error_slot); throw; }
        process.release_guest(error_slot);
        if (document && display) {
            if (send(document, "windowControllers").x0 && !send(send(document, "windowControllers").x0, "count").x0)
                static_cast<void>(send(document, "makeWindowControllers"));
            static_cast<void>(send(document, "showWindows"));
        }
        if (handler) static_cast<void>(invoke_block(handler, {document, static_cast<Address>(already), document ? 0 : error}));
    }, "v@:@B@?");
    // The open panel over every document type's extensions.
    method(dc, "URLsFromRunningOpenPanel", [this, document_types](CpuState& s, GuestMemory&) {
        const auto panel = send(objc.find_class("NSOpenPanel"), "openPanel").x0;
        static_cast<void>(send_id(panel, "setAllowsMultipleSelection:", {1}));
        static_cast<void>(send_id(panel, "setCanChooseFiles:", {1}));
        std::vector<Address> extensions;
        for (const auto& type : document_types())
            for (const auto& extension : type.extensions) extensions.push_back(auto_string(extension));
        if (!extensions.empty()) static_cast<void>(send_id(panel, "setAllowedFileTypes:", {auto_array(extensions)}));
        s.x[0] = send(panel, "runModal").x0 == 1 ? send(panel, "URLs").x0 : 0;
    });
    method(dc, "openDocument:", [this](CpuState& s, GuestMemory&) {
        const auto urls = send(s.x[0], "URLsFromRunningOpenPanel").x0;
        for (const auto url : items(urls)) {
            CallRegs registers;
            registers.x[2] = url;
            registers.x[3] = 1;
            registers.x[4] = 0;
            static_cast<void>(send(s.x[0], "openDocumentWithContentsOfURL:display:completionHandler:", registers));
        }
    }, "v@:@");
    method(dc, "closeAllDocumentsWithDelegate:didCloseAllSelector:contextInfo:", [this, dcs](CpuState& s, GuestMemory&) {
        const auto documents = dcs(s.x[0])->documents;
        bool all = true;
        for (const auto document : documents) {
            if (send_bool(document, "isDocumentEdited")) { all = false; continue; }   // unsaved changes stay open
            static_cast<void>(send(document, "close"));
        }
        if (s.x[2] && s.x[3]) {
            CallRegs registers;
            registers.x[2] = s.x[0];
            registers.x[3] = all;
            registers.x[4] = s.x[4];
            static_cast<void>(objc.send(s.x[2], s.x[3], registers));
        }
    }, "v@:@:^v");
    method(dc, "autosavingDelay", [dcs](CpuState& s, GuestMemory&) { set_d(s, 0, dcs(s.x[0])->autosaving_delay); }, "d@:");
    method(dc, "setAutosavingDelay:", [dcs](CpuState& s, GuestMemory&) { dcs(s.x[0])->autosaving_delay = d(s, 0); }, "v@:d");
    method(dc, "validateUserInterfaceItem:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    method(dc, "dealloc", [this, dc](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<DocumentControllerState>(s.x[0]))
            for (const auto document : std::exchange(state->documents, {})) release(document);
        static_cast<void>(objc.send_super(s.x[0], dc, sel("dealloc")));
    }, "v@:");

    // ---- NSDocument ----
    const auto d = document_class;
    method(d, "init", [this, d, ds](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], d, sel("init")).x0;
        s.x[0] = self;
        if (self) static_cast<void>(ds(self));
    });
    method(d, "initWithType:error:", [this](CpuState& s, GuestMemory&) {
        const auto self = send(s.x[0], "init").x0;
        s.x[0] = self;
        if (self) send_id(self, "setFileType:", {s.x[2]});
    }, "@@:@^@");
    method(d, "initWithContentsOfURL:ofType:error:", [this](CpuState& s, GuestMemory&) {
        const auto self = send(s.x[0], "init").x0;
        s.x[0] = self;
        if (!self) return;
        send_id(self, "setFileURL:", {s.x[2]});
        send_id(self, "setFileType:", {s.x[3]});
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        registers.x[4] = s.x[4];
        if (!(send(self, "readFromURL:ofType:error:", registers).x0 & 0xff)) { release(self); s.x[0] = 0; }
    }, "@@:@@^@");
    // Files: their bytes to -readFromData:ofType:error:. Packages need NSFileWrapper.
    method(d, "readFromURL:ofType:error:", [this, cocoa_error, set_error](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? cf.url_path(s.x[2]) : std::string();
        if (cf.url_is_directory(s.x[2])) unsupported("NSDocument reading of a file package (" + path + "; NSFileWrapper)");
        const auto bytes = read_file(path);
        if (!bytes) { set_error(s.x[4], cocoa_error(260, "The file " + path + " couldn't be opened.")); s.x[0] = 0; return; }
        const auto data_class = objc.find_class("NSData");
        const auto block = process.allocate_guest(std::max<std::size_t>(bytes->size(), 1));
        Address value{};
        try {
            if (!bytes->empty()) memory().write_bytes(block, *bytes);
            CallRegs make;
            make.x[2] = block;
            make.x[3] = bytes->size();
            value = send(data_class, "dataWithBytes:length:", make).x0;
        } catch (...) { process.release_guest(block); throw; }
        process.release_guest(block);
        CallRegs registers;
        registers.x[2] = value;
        registers.x[3] = s.x[3];
        registers.x[4] = s.x[4];
        s.x[0] = send(s.x[0], "readFromData:ofType:error:", registers).x0 & 0xff;
    }, "B@:@@^@");
    method(d, "readFromData:ofType:error:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSDocument readFromData:ofType:error:] (the subclass must override it)");
    }, "B@:@@^@");
    for (const auto* name : {"revertDocumentToSaved:", "runPageLayout:", "printDocument:"})
        method(d, name, [name](CpuState&, GuestMemory&) { unsupported(std::string("-[NSDocument ") + name + "] (reverting and printing)"); }, "v@:@");
    // ---- saving ----
    // NSSaveOperation 0, NSSaveAsOperation 1, NSSaveToOperation 2, NSAutosaveInPlaceOperation 4.
    method(d, "dataOfType:error:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSDocument dataOfType:error:] (the subclass must override it or -writeToURL:ofType:error:)");
    }, "@@:@^@");
    method(d, "writeToURL:ofType:error:", [this, cocoa_error, set_error](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], url = s.x[2], type = s.x[3], out = s.x[4];
        if (!url) fail("-[NSDocument writeToURL:ofType:error:] without a URL");
        // NSDocument itself has no -fileWrapperOfType:error:, so a receiver
        // that answers it is a subclass saving file wrappers.
        if (responds(self, "fileWrapperOfType:error:")) {
            // A subclass that saves file packages: the wrapper writes itself.
            const auto wrapper = send_id(self, "fileWrapperOfType:error:", {type, out});
            if (!wrapper) { s.x[0] = 0; return; }
            CallRegs registers;
            registers.x[2] = url;
            registers.x[3] = 1;   // NSFileWrapperWritingAtomic
            registers.x[4] = 0;
            registers.x[5] = out;
            s.x[0] = send(wrapper, "writeToURL:options:originalContentsURL:error:", registers).x0 & 0xff;
            return;
        }
        const auto data = send_id(self, "dataOfType:error:", {type, out});
        if (!data) { s.x[0] = 0; return; }
        const auto bytes = data_bytes(data);
        const std::filesystem::path target(utf8_path(host_path(cf.url_path(url))));
        auto temporary = target;
        temporary += ".alchemy-saving";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (file) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                set_error(out, cocoa_error(513, "The document could not be saved to " + cf.url_path(url) + "."));
                s.x[0] = 0;
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            set_error(out, cocoa_error(513, "The document could not be saved to " + cf.url_path(url) + "."));
            s.x[0] = 0;
            return;
        }
        s.x[0] = 1;
    }, "B@:@@^@");
    method(d, "writeToURL:ofType:forSaveOperation:originalContentsURL:error:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "writeToURL:ofType:error:", {s.x[2], s.x[3], s.x[6]}) & 0xff;
    }, "B@:@@Q@^@");
    // The file written atomically (writeToURL:ofType:error: writes a
    // temporary file and moves it over the target).
    method(d, "writeSafelyToURL:ofType:forSaveOperation:error:", [this, ds](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        registers.x[4] = s.x[4];
        registers.x[5] = ds(s.x[0])->file_url;
        registers.x[6] = s.x[5];
        s.x[0] = send(s.x[0], "writeToURL:ofType:forSaveOperation:originalContentsURL:error:", registers).x0 & 0xff;
    }, "B@:@@Q^@");
    // One save: write, then adopt the URL and type (save / save as) and
    // clear the change count; returns the error (nil on success).
    const auto save_to = [this, ds](Address self, Address url, Address type, Address operation) -> std::pair<bool, Address> {
        if (operation == 4 || operation == 3) unsupported("NSDocument autosaving (NSAutosaveElsewhere / InPlace operations)");
        const auto error_slot = process.allocate_guest(8);
        memory().write64(error_slot, 0);
        CallRegs registers;
        registers.x[2] = url;
        registers.x[3] = type;
        registers.x[4] = operation;
        registers.x[5] = error_slot;
        bool ok{};
        try { ok = send(self, "writeSafelyToURL:ofType:forSaveOperation:error:", registers).x0 & 0xff; }
        catch (...) { process.release_guest(error_slot); throw; }
        const auto error = memory().read64(error_slot);
        process.release_guest(error_slot);
        if (ok && operation != 2) {
            static_cast<void>(send_id(self, "setFileURL:", {url}));
            static_cast<void>(send_id(self, "setFileType:", {type}));
            static_cast<void>(send_id(self, "updateChangeCount:", {2}));   // NSChangeCleared
            ds(self)->draft = false;
        }
        return {ok, ok ? 0 : error};
    };
    // -document:didSave:contextInfo: of the delegate.
    const auto tell_delegate = [this](Address self, Address delegate, Address selector, bool saved, Address context) {
        if (!delegate || !selector) return;
        CallRegs registers;
        registers.x[2] = self;
        registers.x[3] = saved;
        registers.x[4] = context;
        static_cast<void>(objc.send(delegate, selector, registers));
    };
    method(d, "saveToURL:ofType:forSaveOperation:completionHandler:", [this, save_to](CpuState& s, GuestMemory&) {
        const auto [ok, error] = save_to(s.x[0], s.x[2], s.x[3], s.x[4]);
        static_cast<void>(ok);
        if (s.x[5]) static_cast<void>(invoke_block(s.x[5], {error}));
    }, "v@:@@Q@?");
    method(d, "saveToURL:ofType:forSaveOperation:delegate:didSaveSelector:contextInfo:", [save_to, tell_delegate](CpuState& s, GuestMemory&) {
        const auto [ok, error] = save_to(s.x[0], s.x[2], s.x[3], s.x[4]);
        static_cast<void>(error);
        tell_delegate(s.x[0], s.x[5], s.x[6], ok, s.x[7]);
    }, "v@:@@Q@:^v");
    // The save panel for a document: its type's extensions, its name.
    const auto run_save_panel = [this, ds, find_type](Address self) -> Address {
        const auto panel = send(objc.find_class("NSSavePanel"), "savePanel").x0;
        std::vector<Address> extensions;
        if (const auto type = ds(self)->file_type)
            if (const auto found = find_type(text(type)))
                for (const auto& extension : found->extensions) extensions.push_back(auto_string(extension));
        if (!extensions.empty()) static_cast<void>(send_id(panel, "setAllowedFileTypes:", {auto_array(extensions)}));
        static_cast<void>(send_id(panel, "setNameFieldStringValue:", {send(self, "displayName").x0}));
        if (const auto url = ds(self)->file_url) {
            auto path = cf.url_path(url);
            path = path.substr(0, path.find_last_of('/'));
            if (!path.empty()) static_cast<void>(send_id(panel, "setDirectoryURL:", {autorelease(cf.file_url(path, true))}));
        }
        return send(panel, "runModal").x0 == 1 ? send(panel, "URL").x0 : 0;
    };
    method(d, "runModalSavePanelForSaveOperation:delegate:didSaveSelector:contextInfo:", [this, ds, run_save_panel, save_to, tell_delegate](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto url = run_save_panel(self);
        bool ok = false;
        if (url) {
            auto type = ds(self)->file_type;
            if (!type) type = send(send(document_controller, "sharedDocumentController").x0, "defaultType").x0;
            ok = save_to(self, url, type, s.x[2]).first;
        }
        tell_delegate(self, s.x[3], s.x[4], ok, s.x[5]);
    }, "v@:Q@:^v");
    method(d, "saveDocumentWithDelegate:didSaveSelector:contextInfo:", [this, ds, save_to, tell_delegate](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ds(self);
        if (!state->file_url || state->draft) {
            CallRegs registers;
            registers.x[2] = 1;   // NSSaveAsOperation
            registers.x[3] = s.x[2];
            registers.x[4] = s.x[3];
            registers.x[5] = s.x[4];
            static_cast<void>(send(self, "runModalSavePanelForSaveOperation:delegate:didSaveSelector:contextInfo:", registers));
            return;
        }
        const auto ok = save_to(self, state->file_url, state->file_type, 0).first;
        tell_delegate(self, s.x[2], s.x[3], ok, s.x[4]);
    }, "v@:@:^v");
    method(d, "saveDocument:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "saveDocumentWithDelegate:didSaveSelector:contextInfo:", {0, 0, 0}));
    }, "v@:@");
    for (const auto& [name, operation] : std::initializer_list<std::pair<const char*, Address>>{{"saveDocumentAs:", 1}, {"saveDocumentTo:", 2}}) {
        const auto op = operation;
        method(d, name, [this, op](CpuState& s, GuestMemory&) {
            CallRegs registers;
            registers.x[2] = op;
            static_cast<void>(send(s.x[0], "runModalSavePanelForSaveOperation:delegate:didSaveSelector:contextInfo:", registers));
        }, "v@:@");
    }
    method(d, "keepBackupFile", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // No backup file is kept (-keepBackupFile is NO).
    method(d, "backupFileURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(d, "autosavedContentsFileURL", [ds](CpuState& s, GuestMemory&) { s.x[0] = ds(s.x[0])->autosaved_url; });
    method(d, "setAutosavedContentsFileURL:", [this, ds](CpuState& s, GuestMemory&) { assign(ds(s.x[0])->autosaved_url, s.x[2]); }, "v@:@");
    method(d, "isDraft", [ds](CpuState& s, GuestMemory&) { return_bool(s, ds(s.x[0])->draft); }, "B@:");
    method(d, "setDraft:", [ds](CpuState& s, GuestMemory&) { ds(s.x[0])->draft = bool_arg(s, 2); }, "v@:B");

    method(d, "fileURL", [ds](CpuState& s, GuestMemory&) { s.x[0] = ds(s.x[0])->file_url; });
    method(d, "setFileURL:", [this, ds](CpuState& s, GuestMemory&) { assign(ds(s.x[0])->file_url, s.x[2]); }, "v@:@");
    method(d, "fileType", [ds](CpuState& s, GuestMemory&) { s.x[0] = ds(s.x[0])->file_type; });
    method(d, "setFileType:", [this, ds](CpuState& s, GuestMemory&) {
        release(std::exchange(ds(s.x[0])->file_type, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(d, "fileModificationDate", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(d, "setFileModificationDate:", [](CpuState&, GuestMemory&) {}, "v@:@");
    // The file name without its extension, else the set name, else "Untitled".
    method(d, "displayName", [this, ds](CpuState& s, GuestMemory&) {
        const auto state = ds(s.x[0]);
        if (state->display_name) { s.x[0] = state->display_name; return; }
        if (state->file_url) {
            auto path = cf.url_path(state->file_url);
            while (path.size() > 1 && path.back() == '/') path.pop_back();
            auto name = path.substr(path.find_last_of('/') + 1);
            if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) name.resize(dot);
            s.x[0] = auto_string(name);
            return;
        }
        s.x[0] = auto_string("Untitled");
    });
    method(d, "setDisplayName:", [this, ds](CpuState& s, GuestMemory&) {
        release(std::exchange(ds(s.x[0])->display_name, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(d, "isDocumentEdited", [ds](CpuState& s, GuestMemory&) { return_bool(s, ds(s.x[0])->change_count != 0); }, "B@:");
    // NSChangeDone 0, NSChangeUndone 1, NSChangeCleared 2, NSChangeReadOtherContents 3,
    // NSChangeAutosaved 4, NSChangeRedone 5; NSChangeDiscardable (256) is a flag.
    method(d, "updateChangeCount:", [this, ds](CpuState& s, GuestMemory&) {
        const auto state = ds(s.x[0]);
        switch (s.x[2] & 0xff) {
        case 0: case 5: ++state->change_count; break;
        case 1: --state->change_count; break;
        case 2: case 3: state->change_count = 0; break;
        default: break;
        }
        for (const auto controller : state->window_controllers)
            if (const auto target = send(controller, "window").x0) send_id(target, "setDocumentEdited:", {static_cast<Address>(state->change_count != 0)});
    }, "v@:Q");
    method(d, "hasUnautosavedChanges", [ds](CpuState& s, GuestMemory&) { return_bool(s, ds(s.x[0])->change_count != 0); }, "B@:");
    class_method(d, "autosavesInPlace", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    class_method(d, "preservesVersions", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    class_method(d, "usesUbiquitousStorage", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    // Undo manager (made on first use while the document has one).
    method(d, "undoManager", [this, ds](CpuState& s, GuestMemory&) {
        const auto state = ds(s.x[0]);
        if (!state->undo_manager && state->has_undo_manager) {
            const auto undo_class = objc.find_class("NSUndoManager");
            if (!undo_class) unsupported("-[NSDocument undoManager] without NSUndoManager");
            state->undo_manager = alloc_init(undo_class);
        }
        s.x[0] = state->undo_manager;
    });
    method(d, "setUndoManager:", [this, ds](CpuState& s, GuestMemory&) { assign(ds(s.x[0])->undo_manager, s.x[2]); }, "v@:@");
    method(d, "hasUndoManager", [ds](CpuState& s, GuestMemory&) { return_bool(s, ds(s.x[0])->has_undo_manager); }, "B@:");
    method(d, "setHasUndoManager:", [this, ds](CpuState& s, GuestMemory&) {
        const auto state = ds(s.x[0]);
        state->has_undo_manager = bool_arg(s, 2);
        if (!state->has_undo_manager) release(std::exchange(state->undo_manager, 0));
    }, "v@:B");
    // Window controllers.
    method(d, "windowNibName", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(d, "makeWindowControllers", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto name = send(self, "windowNibName").x0;
        if (!name) return;
        const auto controller = send_id(send(window_controller, "alloc").x0, "initWithWindowNibName:owner:", {name, self});
        send_id(self, "addWindowController:", {controller});
        release(controller);
    }, "v@:");
    method(d, "windowControllers", [this, ds](CpuState& s, GuestMemory&) { s.x[0] = auto_array(ds(s.x[0])->window_controllers); });
    method(d, "addWindowController:", [this, ds](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        auto& list = ds(s.x[0])->window_controllers;
        if (std::find(list.begin(), list.end(), s.x[2]) == list.end()) list.push_back(retain(s.x[2]));
        if (send(s.x[2], "document").x0 != s.x[0]) send_id(s.x[2], "setDocument:", {s.x[0]});
    }, "v@:@");
    method(d, "removeWindowController:", [this, ds](CpuState& s, GuestMemory&) {
        auto& list = ds(s.x[0])->window_controllers;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        if (send(s.x[2], "document").x0 == s.x[0]) send_id(s.x[2], "setDocument:", {0});
        release(s.x[2]);
    }, "v@:@");
    // The nib's `window` outlet (File's Owner is the document): the window of
    // the controller loading the nib.
    method(d, "setWindow:", [this, ds](CpuState& s, GuestMemory&) {
        for (const auto controller : ds(s.x[0])->window_controllers)
            if (objc.host_state<ControllerState>(controller)->loading) { send_id(controller, "setWindow:", {s.x[2]}); return; }
        fail("-[NSDocument setWindow:] outside a window controller's nib loading");
    }, "v@:@");
    method(d, "windowControllerWillLoadNib:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(d, "windowControllerDidLoadNib:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(d, "showWindows", [this, ds](CpuState& s, GuestMemory&) {
        for (const auto controller : ds(s.x[0])->window_controllers) send_id(controller, "showWindow:", {s.x[0]});
    }, "v@:");
    method(d, "windowForSheet", [this, ds](CpuState& s, GuestMemory&) {
        const auto& list = ds(s.x[0])->window_controllers;
        s.x[0] = list.empty() ? 0 : send(list.front(), "window").x0;
    });
    // Closes the windows and leaves the document controller.
    method(d, "close", [this, ds](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ds(self);
        if (state->closing) return;
        state->closing = true;
        const auto keep = retain(self);
        for (const auto controller : std::vector<Address>(state->window_controllers)) {
            static_cast<void>(send(controller, "close"));
            send_id(self, "removeWindowController:", {controller});
        }
        if (const auto controller = send(objc.find_class("NSDocumentController"), "sharedDocumentController").x0)
            send_id(controller, "removeDocument:", {self});
        release(keep);
    }, "v@:");
    method(d, "canCloseDocumentWithDelegate:shouldCloseSelector:contextInfo:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) return;
        CallRegs registers;
        registers.x[2] = s.x[0];
        registers.x[3] = !send_bool(s.x[0], "isDocumentEdited");
        registers.x[4] = s.x[4];
        static_cast<void>(objc.send(s.x[2], s.x[3], registers));
    }, "v@:@:^v");
    method(d, "validateUserInterfaceItem:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    method(d, "dealloc", [this, d](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<DocumentState>(s.x[0])) {
            for (const auto controller : std::exchange(state->window_controllers, {})) release(controller);
            for (auto* slot : {&state->file_url, &state->file_type, &state->display_name, &state->undo_manager, &state->autosaved_url}) release(std::exchange(*slot, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], d, sel("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::appkit
