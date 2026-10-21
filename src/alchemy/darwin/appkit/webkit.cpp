#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include <map>

// WebKit: the framework identity (QLab's changelog window and Sparkle's
// release notes link it) with real configuration objects and views, and no
// web engine.
//   WKWebViewConfiguration, WKPreferences, WKProcessPool,
//   WKUserContentController, WKUserScript, WKWebsiteDataStore and the legacy
//   WebPreferences keep their properties (WebKit's defaults), copy and decode
//   from nibs (the keys Interface Builder archives);
//   WKWebView, WKFlippedView and the legacy WebView are NSViews (decodable,
//   placeable, drawn as empty views) with their configuration, delegates and
//   main frame (WebFrame).
// Loading or evaluating any content (-loadHTMLString:baseURL:,
// -loadData:..., -loadFileURL:..., -loadRequest:, JavaScript) fails
// explicitly: Alchemy has no HTML engine.
namespace alchemy::gui::completion_kit::graphics {
namespace {

// Host state of the WebKit views. NSView instances already carry AppKit's
// host state, so the views' own values live in this side table, keyed by
// the view and dropped in -dealloc.
struct ViewValues {
    Address configuration{}, preferences{}, main_frame{};   // +1
    Address navigation_delegate{}, ui_delegate{}, frame_load_delegate{}, policy_delegate{};   // weak
    bool draws_background{true};
};
struct WebKitViews {
    std::mutex mutex;
    std::map<Address, ViewValues> views;
};

[[noreturn]] void no_engine(std::string_view what) {
    unsupported("WebKit " + std::string(what) + ": Alchemy has no HTML engine, so web content cannot be loaded or shown");
}

} // namespace

void install_webkit(Support& support) {
    ProcessContext::add_provider_library(std::string(webkit_library));
    auto* sp = &support;
    const auto key = [sp](const char* name) { return sp->cf.constant_string(name); };
    // -initWithCoder: helpers for nib / keyed decoding.
    const auto decode_object = [sp, key](Address coder, const char* name) { return sp->send_id(coder, "decodeObjectForKey:", {key(name)}); };
    const auto has = [sp, key](Address coder, const char* name) {
        return (sp->send_id(coder, "containsValueForKey:", {key(name)}) & 0xff) != 0;
    };
    const auto decode_bool = [sp, key](Address coder, const char* name) {
        return (sp->send_id(coder, "decodeBoolForKey:", {key(name)}) & 0xff) != 0;
    };
    const auto decode_double = [sp, key](Address coder, const char* name) {
        CallRegs registers;
        registers.x[2] = key(name);
        return sp->send(coder, "decodeDoubleForKey:", registers).d(0);
    };
    const auto make = [sp](const char* class_name) {
        return sp->send_id(sp->send_id(sp->objc.find_class(class_name), "alloc"), "init");   // +1
    };

    // ---- WKPreferences -------------------------------------------------------------------------
    const auto preferences = support.declare_class("WKPreferences", "NSObject", webkit_library);
    Props(support, preferences)
        .lifecycle()
        .real("minimumFontSize", "setMinimumFontSize:")
        .boolean("javaScriptEnabled", "setJavaScriptEnabled:", true)
        .boolean("javaScriptCanOpenWindowsAutomatically", "setJavaScriptCanOpenWindowsAutomatically:", false)
        .boolean("tabFocusesLinks", "setTabFocusesLinks:", false)
        .boolean("shouldPrintBackgrounds", "setShouldPrintBackgrounds:", false)
        .boolean("isTextInteractionEnabled", "setTextInteractionEnabled:", true)
        .boolean("isFraudulentWebsiteWarningEnabled", "setFraudulentWebsiteWarningEnabled:", true)
        .boolean("isElementFullscreenEnabled", "setElementFullscreenEnabled:", false)
        .boolean("javaEnabled", "setJavaEnabled:", false)
        .boolean("plugInsEnabled", "setPlugInsEnabled:", false);
    support.method(preferences, "initWithCoder:", [sp, has, decode_bool, decode_double](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        for (const auto* name : {"javaScriptEnabled", "javaScriptCanOpenWindowsAutomatically", "tabFocusesLinks",
                                 "shouldPrintBackgrounds"})
            if (has(coder, name)) bag_set_raw(*sp, self, name, {decode_bool(coder, name) ? 1ULL : 0ULL, 0, 0, 0});
        if (has(coder, "textInteractionEnabled"))
            bag_set_raw(*sp, self, "isTextInteractionEnabled", {decode_bool(coder, "textInteractionEnabled") ? 1ULL : 0ULL, 0, 0, 0});
        if (has(coder, "minimumFontSize"))
            bag_set_raw(*sp, self, "minimumFontSize", {std::bit_cast<std::uint64_t>(decode_double(coder, "minimumFontSize")), 0, 0, 0});
        s.x[0] = self;
    }, "@@:@");

    // ---- WKProcessPool, WKWebsiteDataStore --------------------------------------------------------
    const auto pool = support.declare_class("WKProcessPool", "NSObject", webkit_library);
    Props(support, pool).lifecycle();
    support.method(pool, "initWithCoder:", [](CpuState&, GuestMemory&) {}, "@@:@");   // no archived state
    const auto store = support.declare_class("WKWebsiteDataStore", "NSObject", webkit_library);
    Props(support, store).lifecycle().boolean("isPersistent", nullptr, true);
    struct Stores { Address persistent{}, ephemeral{}; };
    auto stores = std::make_shared<Stores>();
    const auto shared_store = [sp, store, stores](bool persistent) {
        auto& slot = persistent ? stores->persistent : stores->ephemeral;
        if (!slot) {
            slot = sp->send_id(sp->send_id(store, "alloc"), "init");
            if (const auto record = sp->process.objects().find(slot)) record->immortal = true;
            bag_set_raw(*sp, slot, "isPersistent", {persistent ? 1ULL : 0ULL, 0, 0, 0});
        }
        return slot;
    };
    support.method(store, "defaultDataStore", [shared_store](CpuState& s, GuestMemory&) { s.x[0] = shared_store(true); }, "@@:", true);
    support.method(store, "nonPersistentDataStore", [sp, store](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(store, "alloc"), "init");
        bag_set_raw(*sp, made, "isPersistent", {0, 0, 0, 0});
        s.x[0] = sp->send_id(made, "autorelease");
    }, "@@:", true);
    // A decoded store is the default (persistent) store or a non-persistent one.
    support.method(store, "initWithCoder:", [sp, has, decode_bool, shared_store](CpuState& s, GuestMemory&) {
        const bool is_default = !has(s.x[2], "isDefaultDataStore") || decode_bool(s.x[2], "isDefaultDataStore");
        if (is_default) {
            sp->release(s.x[0]);
            s.x[0] = sp->retain(shared_store(true));
        } else {
            bag_set_raw(*sp, s.x[0], "isPersistent", {0, 0, 0, 0});
        }
    }, "@@:@");

    // ---- WKUserScript, WKUserContentController ------------------------------------------------
    const auto script = support.declare_class("WKUserScript", "NSObject", webkit_library);
    Props(support, script)
        .lifecycle()
        .object("source", nullptr)
        .integer("injectionTime", nullptr)
        .boolean("isForMainFrameOnly", nullptr, true);
    support.method(script, "initWithSource:injectionTime:forMainFrameOnly:", [sp](CpuState& s, GuestMemory&) {
        bag_set_object(*sp, s.x[0], "source", s.x[2], true);
        bag_set_raw(*sp, s.x[0], "injectionTime", {s.x[3], 0, 0, 0});
        bag_set_raw(*sp, s.x[0], "isForMainFrameOnly", {flag(s, 4) ? 1ULL : 0ULL, 0, 0, 0});
    }, "@@:@qB");
    const auto controller = support.declare_class("WKUserContentController", "NSObject", webkit_library);
    Props(support, controller).lifecycle();
    const auto mutable_list = [sp](Address self, const char* name) {
        auto list = bag_object(*sp, self, name);
        if (!list) {
            const auto made = sp->cf.array({}, true);
            bag_set_object(*sp, self, name, made);
            sp->release(made);
            list = bag_object(*sp, self, name);
        }
        return list;
    };
    support.method(controller, "userScripts", [sp](CpuState& s, GuestMemory&) {
        const auto list = bag_object(*sp, s.x[0], "userScripts");
        s.x[0] = list ? sp->send_id(sp->send_id(list, "copy"), "autorelease") : sp->send_id(sp->cf.array({}), "autorelease");
    }, "@@:");
    support.method(controller, "addUserScript:", [sp, mutable_list](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[WKUserContentController addUserScript:] with nil");
        sp->send_id(mutable_list(s.x[0], "userScripts"), "addObject:", {s.x[2]});
    }, "v@:@");
    support.method(controller, "removeAllUserScripts", [sp](CpuState& s, GuestMemory&) {
        bag_set_object(*sp, s.x[0], "userScripts", 0);
    }, "v@:");
    // Script message handlers: recorded by name (no page ever posts one).
    support.method(controller, "addScriptMessageHandler:name:", [sp](CpuState& s, GuestMemory&) {
        const auto name = sp->cf.utf8(s.x[3]);
        if (bag_object(*sp, s.x[0], "handler:" + name))
            fail("-[WKUserContentController addScriptMessageHandler:name:] a handler named '" + name + "' already exists");
        bag_set_object(*sp, s.x[0], "handler:" + name, s.x[2]);
    }, "v@:@@");
    support.method(controller, "removeScriptMessageHandlerForName:", [sp](CpuState& s, GuestMemory&) {
        bag_set_object(*sp, s.x[0], "handler:" + sp->cf.utf8(s.x[2]), 0);
    }, "v@:@");
    support.method(controller, "initWithCoder:", [](CpuState&, GuestMemory&) {}, "@@:@");

    // ---- WKWebViewConfiguration ------------------------------------------------------------------
    const auto configuration = support.declare_class("WKWebViewConfiguration", "NSObject", webkit_library);
    // Lazily made defaults, as WebKit creates them on first access.
    const auto lazy = [sp, make](const char* name, const char* class_name) {
        return [sp, make, name, class_name](CpuState& s, GuestMemory&) {
            auto value = bag_object(*sp, s.x[0], name);
            if (!value) {
                const auto made = make(class_name);
                bag_set_object(*sp, s.x[0], name, made);
                sp->release(made);
                value = made;
            }
            s.x[0] = value;
        };
    };
    Props(support, configuration)
        .lifecycle()
        // The getters (lazily made defaults) are below.
        .object_setter("preferences", "setPreferences:")
        .object_setter("userContentController", "setUserContentController:")
        .object_setter("processPool", "setProcessPool:")
        .object_setter("websiteDataStore", "setWebsiteDataStore:")
        .object("applicationNameForUserAgent", "setApplicationNameForUserAgent:", true)
        .boolean("suppressesIncrementalRendering", "setSuppressesIncrementalRendering:", false)
        .boolean("allowsAirPlayForMediaPlayback", "setAllowsAirPlayForMediaPlayback:", true)
        .integer("userInterfaceDirectionPolicy", "setUserInterfaceDirectionPolicy:", 0)
        .boolean("_drawsBackground", "_setDrawsBackground:", true);
    support.method(configuration, "preferences", lazy("preferences", "WKPreferences"), "@@:");
    support.method(configuration, "userContentController", lazy("userContentController", "WKUserContentController"), "@@:");
    support.method(configuration, "processPool", lazy("processPool", "WKProcessPool"), "@@:");
    support.method(configuration, "websiteDataStore", [sp, shared_store](CpuState& s, GuestMemory&) {
        const auto value = bag_object(*sp, s.x[0], "websiteDataStore");
        s.x[0] = value ? value : shared_store(true);
    }, "@@:");
    support.method(configuration, "initWithCoder:", [sp, has, decode_object, decode_bool](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        for (const auto* name : {"preferences", "userContentController", "processPool", "websiteDataStore", "applicationNameForUserAgent"})
            if (has(coder, name)) bag_set_object(*sp, self, name, decode_object(coder, name));
        for (const auto* name : {"suppressesIncrementalRendering", "allowsAirPlayForMediaPlayback"})
            if (has(coder, name)) bag_set_raw(*sp, self, name, {decode_bool(coder, name) ? 1ULL : 0ULL, 0, 0, 0});
        if (has(coder, "drawsBackground"))
            bag_set_raw(*sp, self, "_drawsBackground", {decode_bool(coder, "drawsBackground") ? 1ULL : 0ULL, 0, 0, 0});
    }, "@@:@");

    // ---- views -----------------------------------------------------------------------------------------
    auto views = std::make_shared<WebKitViews>();
    const auto values = [views](Address view) -> ViewValues& { return views->views[view]; };
    const auto view_dealloc = [sp, views](Address cls) {
        sp->method(cls, "dealloc", [sp, views, cls](CpuState& s, GuestMemory&) {
            ViewValues gone;
            {
                std::lock_guard lock(views->mutex);
                if (const auto it = views->views.find(s.x[0]); it != views->views.end()) { gone = it->second; views->views.erase(it); }
            }
            for (const auto object : {gone.configuration, gone.preferences, gone.main_frame}) sp->release(object);
            static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
        }, "v@:");
    };
    const auto view_object = [sp, views, values](Address cls, const char* getter, const char* setter, Address ViewValues::*member, bool strong) {
        if (getter)
            sp->method(cls, getter, [views, values, member](CpuState& s, GuestMemory&) {
                std::lock_guard lock(views->mutex);
                s.x[0] = values(s.x[0]).*member;
            }, "@@:");
        if (!setter) return;
        sp->method(cls, setter, [sp, views, values, member, strong](CpuState& s, GuestMemory&) {
            const auto value = strong ? sp->retain(s.x[2]) : s.x[2];
            Address old{};
            {
                std::lock_guard lock(views->mutex);
                old = std::exchange(values(s.x[0]).*member, value);
            }
            if (strong) sp->release(old);
        }, "v@:@");
    };

    const auto flipped = support.declare_class("WKFlippedView", "NSView", webkit_library);
    support.method(flipped, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");

    const auto web_view = support.declare_class("WKWebView", "NSView", webkit_library);
    view_dealloc(web_view);
    view_object(web_view, "navigationDelegate", "setNavigationDelegate:", &ViewValues::navigation_delegate, false);
    view_object(web_view, "UIDelegate", "setUIDelegate:", &ViewValues::ui_delegate, false);
    // The configuration is copied, as WebKit does.
    const auto adopt_configuration = [sp, views, values, make](Address self, Address given) {
        const auto copy = given ? sp->send_id(given, "copy") : make("WKWebViewConfiguration");
        Address old{};
        {
            std::lock_guard lock(views->mutex);
            old = std::exchange(values(self).configuration, copy);
        }
        sp->release(old);
    };
    support.method(web_view, "configuration", [sp, views, values](CpuState& s, GuestMemory&) {
        Address value{};
        { std::lock_guard lock(views->mutex); value = values(s.x[0]).configuration; }
        s.x[0] = value ? sp->send_id(sp->send_id(value, "copy"), "autorelease") : 0;
    }, "@@:");
    support.method(web_view, "initWithFrame:configuration:", [sp, web_view, adopt_configuration](CpuState& s, GuestMemory&) {
        const auto configuration = s.x[2];
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.v[i] = s.v[i];
        const auto self = sp->objc.send_super(s.x[0], web_view, sp->objc.selector("initWithFrame:"), registers).x0;
        if (self) adopt_configuration(self, configuration);
        s.x[0] = self;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    support.method(web_view, "initWithFrame:", [sp, web_view, adopt_configuration](CpuState& s, GuestMemory&) {
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.v[i] = s.v[i];
        const auto self = sp->objc.send_super(s.x[0], web_view, sp->objc.selector("initWithFrame:"), registers).x0;
        if (self) adopt_configuration(self, 0);
        s.x[0] = self;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(web_view, "initWithCoder:", [sp, web_view, adopt_configuration, decode_object](CpuState& s, GuestMemory&) {
        const auto coder = s.x[2];
        CallRegs registers;
        registers.x[2] = coder;
        const auto self = sp->objc.send_super(s.x[0], web_view, sp->objc.selector("initWithCoder:"), registers).x0;
        if (self) adopt_configuration(self, decode_object(coder, "configuration"));
        s.x[0] = self;
    }, "@@:@");
    support.method(web_view, "_setDrawsBackground:", [views, values](CpuState& s, GuestMemory&) {
        std::lock_guard lock(views->mutex);
        values(s.x[0]).draws_background = flag(s, 2);
    }, "v@:B");
    support.method(web_view, "_drawsBackground", [views, values](CpuState& s, GuestMemory&) {
        std::lock_guard lock(views->mutex);
        put_bool(s, values(s.x[0]).draws_background);
    }, "B@:");
    // Nothing is ever loaded: no URL, title, history or progress.
    for (const auto* name : {"URL", "title", "backForwardList", "serverTrust"})
        support.method(web_view, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    for (const auto* name : {"isLoading", "canGoBack", "canGoForward", "hasOnlySecureContent"})
        support.method(web_view, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(web_view, "estimatedProgress", [](CpuState& s, GuestMemory&) { put_d(s, 0, 0); }, "d@:");
    support.method(web_view, "stopLoading", [](CpuState&, GuestMemory&) {}, "v@:");
    for (const auto* name : {"loadHTMLString:baseURL:", "loadData:MIMEType:characterEncodingName:baseURL:",
                             "loadFileURL:allowingReadAccessToURL:", "loadRequest:", "evaluateJavaScript:completionHandler:",
                             "reload", "reloadFromOrigin", "goBack", "goForward"}) {
        const std::string what = std::string("-[WKWebView ") + name + "]";
        support.method(web_view, name, [what](CpuState&, GuestMemory&) { no_engine(what); }, "@@:@@");
    }

    // ---- legacy WebKit: WebPreferences, WebView, WebFrame -----------------------------------------
    const auto web_preferences = support.declare_class("WebPreferences", "NSObject", webkit_library);
    Props(support, web_preferences)
        .lifecycle()
        .object("identifier", nullptr)
        .object("standardFontFamily", "setStandardFontFamily:", true, [key] { return key("Times"); })
        .object("userStyleSheetLocation", "setUserStyleSheetLocation:", true)
        .integer("defaultFontSize", "setDefaultFontSize:", 16)
        .boolean("allowsAnimatedImageLooping", "setAllowsAnimatedImageLooping:", true)
        .boolean("allowsAnimatedImages", "setAllowsAnimatedImages:", true)
        .boolean("isJavaEnabled", "setJavaEnabled:", false)
        .boolean("isJavaScriptEnabled", "setJavaScriptEnabled:", true)
        .boolean("javaScriptCanOpenWindowsAutomatically", "setJavaScriptCanOpenWindowsAutomatically:", true)
        .boolean("loadsImagesAutomatically", "setLoadsImagesAutomatically:", true)
        .boolean("arePlugInsEnabled", "setPlugInsEnabled:", false)
        .boolean("userStyleSheetEnabled", "setUserStyleSheetEnabled:", false)
        .boolean("autosaves", "setAutosaves:", false);
    auto standard = std::make_shared<Address>(0);
    support.method(web_preferences, "standardPreferences", [sp, web_preferences, standard](CpuState& s, GuestMemory&) {
        if (!*standard) {
            *standard = sp->send_id(sp->send_id(web_preferences, "alloc"), "init");
            if (const auto record = sp->process.objects().find(*standard)) record->immortal = true;
        }
        s.x[0] = *standard;
    }, "@@:", true);
    support.method(web_preferences, "initWithIdentifier:", [sp](CpuState& s, GuestMemory&) {
        bag_set_object(*sp, s.x[0], "identifier", s.x[2], true);
    }, "@@:@");

    const auto frame = support.declare_class("WebFrame", "NSObject", webkit_library);
    Props(support, frame).lifecycle();
    support.method(frame, "webView", [sp](CpuState& s, GuestMemory&) { s.x[0] = bag_integer(*sp, s.x[0], "webView").value_or(0); }, "@@:");
    support.method(frame, "parentFrame", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // the main frame
    support.method(frame, "name", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->cf.constant_string(""); }, "@@:");
    support.method(frame, "dataSource", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(frame, "stopLoading", [](CpuState&, GuestMemory&) {}, "v@:");
    for (const auto* name : {"loadData:MIMEType:textEncodingName:baseURL:", "loadHTMLString:baseURL:", "loadRequest:",
                             "loadAlternateHTMLString:baseURL:forUnreachableURL:", "reload"}) {
        const std::string what = std::string("-[WebFrame ") + name + "]";
        support.method(frame, name, [what](CpuState&, GuestMemory&) { no_engine(what); }, "v@:@@@@");
    }

    const auto legacy = support.declare_class("WebView", "NSView", webkit_library);
    view_dealloc(legacy);
    view_object(legacy, "frameLoadDelegate", "setFrameLoadDelegate:", &ViewValues::frame_load_delegate, false);
    view_object(legacy, "policyDelegate", "setPolicyDelegate:", &ViewValues::policy_delegate, false);
    view_object(legacy, "UIDelegate", "setUIDelegate:", &ViewValues::ui_delegate, false);
    view_object(legacy, nullptr, "setPreferences:", &ViewValues::preferences, true);   // -preferences below
    support.method(legacy, "preferences", [sp, views, values](CpuState& s, GuestMemory&) {
        Address value{};
        { std::lock_guard lock(views->mutex); value = values(s.x[0]).preferences; }
        s.x[0] = value ? value : sp->send_id(sp->objc.find_class("WebPreferences"), "standardPreferences");
    }, "@@:");
    support.method(legacy, "mainFrame", [sp, views, values, frame](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        Address value{};
        { std::lock_guard lock(views->mutex); value = values(self).main_frame; }
        if (!value) {
            value = sp->send_id(sp->send_id(frame, "alloc"), "init");
            bag_set_raw(*sp, value, "webView", {self, 0, 0, 0});   // the frame does not retain its view
            std::lock_guard lock(views->mutex);
            values(self).main_frame = value;
        }
        s.x[0] = value;
    }, "@@:");
    support.method(legacy, "initWithFrame:frameName:groupName:", [sp, legacy](CpuState& s, GuestMemory&) {
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.v[i] = s.v[i];
        s.x[0] = sp->objc.send_super(s.x[0], legacy, sp->objc.selector("initWithFrame:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    support.method(legacy, "_setDrawsTransparentBackground:", [views, values](CpuState& s, GuestMemory&) {
        std::lock_guard lock(views->mutex);
        values(s.x[0]).draws_background = !flag(s, 2);
    }, "v@:B");
    support.method(legacy, "setDrawsBackground:", [views, values](CpuState& s, GuestMemory&) {
        std::lock_guard lock(views->mutex);
        values(s.x[0]).draws_background = flag(s, 2);
    }, "v@:B");
    support.method(legacy, "drawsBackground", [views, values](CpuState& s, GuestMemory&) {
        std::lock_guard lock(views->mutex);
        put_bool(s, values(s.x[0]).draws_background);
    }, "B@:");
    support.method(legacy, "stopLoading:", [](CpuState&, GuestMemory&) {}, "v@:@");
    support.method(legacy, "isLoading", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(legacy, "mainFrameURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(legacy, "setMainFrameURL:", [](CpuState&, GuestMemory&) { no_engine("-[WebView setMainFrameURL:]"); }, "v@:@");

    // String constants Sparkle and QLab bind.
    for (const auto* name : {"WebActionNavigationTypeKey", "WebActionElementKey", "WebActionButtonKey",
                             "WebActionModifierFlagsKey", "WebActionOriginalURLKey", "WebElementLinkURLKey"}) {
        const auto symbol = "_" + std::string(name);
        if (!support.process.resolve(webkit_library, symbol)) support.string_constant(webkit_library, symbol, name);
    }
}

} // namespace alchemy::gui::completion_kit::graphics
