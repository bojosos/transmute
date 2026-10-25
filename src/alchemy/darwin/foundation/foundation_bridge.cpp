#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin_version.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace alchemy {
struct FoundationBridge::Impl {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    const std::vector<std::string> initial_arguments;
    const std::vector<std::string> initial_environment;
    const std::string process_name;
    std::mutex pool_metadata_mutex;
    std::shared_ptr<void> extra;
    struct PoolState final : ObjectPayload {
        std::mutex mutex;
        std::uint64_t owner{}, token{};
    };

    Impl(ProcessContext& context, ObjcRuntime& runtime, CoreFoundation& foundation)
        : process(context), objc(runtime), cf(foundation), initial_arguments(context.arguments),
          initial_environment(context.environment), process_name(filename(context.executable)) {
        const auto root = objc.find_class("NSObject");
        if (!root) throw std::runtime_error("Foundation bridge requires the NSObject runtime class");
        objc.publish_class("NSObject", root, foundation_library);
        // NSString is built in CoreFoundation's ___CFConstantStringClassReference,
        // so constant strings are NSStrings and CoreFoundation keeps the export.
        const auto constant_class = process.resolve(core_foundation_library, "___CFConstantStringClassReference");
        const auto strings = make_class("NSString", root, ObjectKind::string, constant_class ? constant_class->address : 0);
        const auto numbers = make_class("NSNumber", root, ObjectKind::number);
        cf.set_class(ObjectKind::boolean, numbers);
        const auto data = make_class("NSData", root, ObjectKind::data);
        const auto arrays = make_class("NSArray", root, ObjectKind::array);
        const auto dictionaries = make_class("NSDictionary", root, ObjectKind::dictionary);
        for (const auto& [name, cls] : {std::pair{"NSArray", arrays}, std::pair{"NSDictionary", dictionaries}})
            objc.publish_class(name, cls, core_foundation_library);
        process.install_export(foundation_library, "___CFConstantStringClassReference",
            {strings, ExportKind::data});

        constructor(strings, "stringWithUTF8String:", "initWithUTF8String:",
            [this](auto& state, auto&) { return call("_CFStringCreateWithCString", {0, state.x[2], 0x08000100}); });
        constructor(strings, "stringWithString:", "initWithString:",
            [this](auto& state, auto&) { return cf.copy(state.x[2]); });
        method(strings, "initWithCharacters:length:", [this](auto& state, auto&) {
            replace_initialized(state, call("_CFStringCreateWithCharacters", {0, state.x[2], state.x[3]}));
        });
        method(strings, "init", [this](auto& state, auto&) { replace_initialized(state, cf.string("")); });
        method(strings, "length", [this](auto& state, auto&) { state.x[0] = cf.length(state.x[0]); });
        method(strings, "UTF8String", [this](auto& state, auto&) { state.x[0] = utf8(state.x[0]); });
        method(strings, "characterAtIndex:", [this](auto& state, auto&) {
            state.x[0] = call("_CFStringGetCharacterAtIndex", {state.x[0], state.x[2]});
        });
        method(strings, "compare:", [this](auto& state, auto&) {
            state.x[0] = call("_CFStringCompare", {state.x[0], state.x[2], 0});
        });
        method(strings, "isEqualToString:", [this](auto& state, auto&) {
            state.x[0] = state.x[2] && cf.equal(state.x[0], state.x[2]);
        });
        method(strings, "stringByAppendingString:", [this](auto& state, auto&) {
            // UTF-16 units concatenated directly (no UTF-8 round trip: Blender
            // assembles its large MSL sources this way).
            auto units = cf.units(state.x[0]);
            const auto tail = cf.units(state.x[2]);
            units.insert(units.end(), tail.begin(), tail.end());
            state.x[0] = autorelease(cf.string_utf16(units));
        });

        constructor(data, "dataWithBytes:length:", "initWithBytes:length:",
            [this](auto& state, auto&) { return call("_CFDataCreate", {0, state.x[2], state.x[3]}); });
        method(data, "init", [this](auto& state, auto&) { replace_initialized(state, call("_CFDataCreate", {0, 0, 0})); });
        method(data, "length", [this](auto& state, auto&) { state.x[0] = cf.length(state.x[0]); });
        method(data, "bytes", [this](auto& state, auto&) { state.x[0] = call("_CFDataGetBytePtr", {state.x[0]}); });
        method(data, "isEqualToData:", [this](auto& state, auto&) {
            state.x[0] = state.x[2] && cf.equal(state.x[0], state.x[2]);
        });

        numeric_constructor(numbers, "numberWithInt:", "initWithInt:", 3, false);
        numeric_constructor(numbers, "numberWithLongLong:", "initWithLongLong:", 4, false);
        numeric_constructor(numbers, "numberWithDouble:", "initWithDouble:", 6, true);
        method(numbers, "init", [](auto&, auto&) {
            throw std::runtime_error("Foundation bridge: NSNumber requires a numeric initializer");
        });
        constructor(numbers, "numberWithBool:", "initWithBool:", [this](auto& state, auto& memory) {
            const auto symbol = state.x[2] ? "_kCFBooleanTrue" : "_kCFBooleanFalse";
            return process.objects().retain(memory.read64(export_address(symbol)));
        });
        method(numbers, "longLongValue", [this](auto& state, auto& memory) { number_value(state, memory, 4, false); });
        method(numbers, "intValue", [this](auto& state, auto& memory) { number_value(state, memory, 3, false); });
        method(numbers, "doubleValue", [this](auto& state, auto& memory) { number_value(state, memory, 6, true); });
        method(numbers, "boolValue", [this](auto& state, auto& memory) {
            if (process.objects().require(state.x[0])->kind == ObjectKind::boolean)
                state.x[0] = call("_CFBooleanGetValue", {state.x[0]});
            else { number_value(state, memory, 4, false); state.x[0] = state.x[0] != 0; }
        });
        method(numbers, "isEqualToNumber:", [this](auto& state, auto&) {
            state.x[0] = state.x[2] && cf.equal(state.x[0], state.x[2]);
        });

        // Construction (+array, +arrayWithObject:, +arrayWithObjects:count:,
        // -init, -initWithObjects:count:) is foundation_extra's, installed at
        // the end of this constructor, as is the dictionaries' below.
        method(arrays, "count", [this](auto& state, auto&) { state.x[0] = cf.length(state.x[0]); });
        method(arrays, "objectAtIndex:", [this](auto& state, auto&) { state.x[0] = cf.element(state.x[0], state.x[2]); });
        method(arrays, "objectAtIndexedSubscript:", [this](auto& state, auto&) { state.x[0] = cf.element(state.x[0], state.x[2]); });

        method(dictionaries, "count", [this](auto& state, auto&) { state.x[0] = cf.length(state.x[0]); });
        method(dictionaries, "objectForKey:", [this](auto& state, auto&) {
            state.x[0] = cf.dictionary_value(state.x[0], state.x[2]);
        });
        method(dictionaries, "objectForKeyedSubscript:", [this](auto& state, auto&) {
            state.x[0] = cf.dictionary_value(state.x[0], state.x[2]);
        });

        for (const auto cls : {strings, numbers, data, arrays, dictionaries}) {
            method(cls, "description", [this](auto& state, auto&) {
                state.x[0] = autorelease(call("_CFCopyDescription", {state.x[0]}));
            });
            method(cls, "isEqual:", [this](auto& state, auto&) { state.x[0] = state.x[2] && cf.equal(state.x[0], state.x[2]); });
            method(cls, "hash", [this](auto& state, auto&) { state.x[0] = cf.hash(state.x[0]); });
            method(cls, "copy", [this](auto& state, auto&) { state.x[0] = cf.copy(state.x[0]); });
            method(cls, "copyWithZone:", [this](auto& state, auto&) {
                if (state.x[2]) throw std::runtime_error("Foundation bridge: custom allocation zones are unsupported");
                state.x[0] = cf.copy(state.x[0]);
            });
        }
        method(root, "description", [this](auto& state, auto&) {
            state.x[0] = autorelease(call("_CFCopyDescription", {state.x[0]}));
        });
        method(root, "description", [this](auto& state, auto& memory) {
            state.x[0] = name_string("_class_getName", state.x[0], memory);
        }, true);
        install_pools(root);
        install_process_info(root);
        foundation_function("_NSLog", [this](auto& state, auto& memory) { log(state.x[0], state.sp, memory); });
        foundation_function("_NSLogv", [this](auto& state, auto& memory) { log(state.x[0], state.x[1], memory); });
        foundation_function("_NSClassFromString", [this](auto& state, auto&) { state.x[0] = objc.find_class(cf.utf8(state.x[0])); });
        foundation_function("_NSSelectorFromString", [this](auto& state, auto&) { state.x[0] = objc.selector(cf.utf8(state.x[0])); });
        foundation_function("_NSStringFromClass", [this](auto& state, auto& memory) {
            state.x[0] = name_string("_class_getName", state.x[0], memory);
        });
        foundation_function("_NSStringFromSelector", [this](auto& state, auto& memory) {
            state.x[0] = name_string("_sel_getName", state.x[0], memory);
        });
        extra = detail::install_foundation_extra(process, objc, cf);
    }
    static std::string filename(const std::filesystem::path& path) {
        const auto bytes = path.filename().u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    static std::uint64_t add(std::uint64_t pointer, std::uint64_t offset) {
        if (pointer > UINT64_MAX - offset) throw std::runtime_error("Foundation guest address overflow");
        return pointer + offset;
    }
    std::uint64_t runtime_call(std::string_view symbol, std::initializer_list<std::uint64_t> arguments,
                               std::string_view library = objc_library) {
        const auto service = process.resolve(library, symbol);
        if (!service) throw std::runtime_error("Foundation bridge: missing service " + std::string(symbol));
        return process.calling_engine().invoke_callback(service->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t native_class(std::string_view name, std::uint64_t root) {
        const auto size = runtime_call("_class_getInstanceSize", {root});
        if (size < 8 || size > UINT32_MAX) throw std::runtime_error("Foundation superclass instance size is unsupported");
        const auto cls = objc.register_native_class(std::string(name), root, static_cast<std::uint32_t>(size));
        objc.publish_class(name, cls, foundation_library);
        return cls;
    }
    std::shared_ptr<PoolState> pool_state(std::uint64_t object, bool required = true) {
        std::lock_guard lock(pool_metadata_mutex);
        const auto record = process.objects().require(object);
        const auto value = std::dynamic_pointer_cast<PoolState>(record->payload);
        if (!value && required) throw std::runtime_error("Foundation NSAutoreleasePool is not initialized");
        return value;
    }
    void install_pools(std::uint64_t root) {
        const auto cls = native_class("NSAutoreleasePool", root);
        method(cls, "init", [this](auto& state, auto&) {
            const auto record = process.objects().require(state.x[0]);
            auto value = std::make_shared<PoolState>();
            value->owner = process.current_thread()->identity;
            {
                std::lock_guard lock(pool_metadata_mutex);
                if (record->payload) throw std::runtime_error("Foundation NSAutoreleasePool initialized twice");
                record->payload = value;
            }
            try {
                const auto token = runtime_call("_objc_autoreleasePoolPush", {});
                std::lock_guard lock(value->mutex);
                value->token = token;
            } catch (...) {
                std::lock_guard lock(pool_metadata_mutex);
                record->payload.reset();
                throw;
            }
        });
        const auto release = [this](auto& state, auto&) {
            const auto value = pool_state(state.x[0], false);
            const auto thread = process.current_thread();
            if (value) {
                std::lock_guard lock(value->mutex);
                if (value->owner != thread->identity) throw std::runtime_error("Foundation NSAutoreleasePool belongs to another thread");
                if (thread->autorelease_pools.empty() || thread->autorelease_pools.back().token != value->token)
                    throw std::runtime_error("Foundation NSAutoreleasePool must be drained in nesting order");
            }
            process.objects().release(state.x[0]);
        };
        method(cls, "drain", release);
        method(cls, "release", release);
        method(cls, "dealloc", [this](auto& state, auto&) {
            const auto value = pool_state(state.x[0], false);
            const auto identity = process.current_thread()->identity;
            std::uint64_t token{};
            if (value) {
                std::lock_guard lock(value->mutex);
                if (value->owner != identity) throw std::runtime_error("Foundation NSAutoreleasePool belongs to another thread");
                token = value->token;
                value->token = 0;
            }
            if (token) static_cast<void>(runtime_call("_objc_autoreleasePoolPop", {token}));
            static_cast<void>(runtime_call("_object_dispose", {state.x[0]}));   // as NSObject's dealloc
        });
        for (const auto name : {"retain", "autorelease"}) method(cls, name, [](auto&, auto&) {
            throw std::runtime_error("Foundation NSAutoreleasePool cannot be retained or autoreleased");
        });
    }
    struct TemporaryObjects {
        ProcessContext& process;
        std::vector<std::uint64_t> values;
        std::uint64_t storage{};
        ~TemporaryObjects() {
            for (const auto value : values) process.objects().release(value);
            if (storage) process.release_guest(storage);
        }
    };
    std::uint64_t arguments_array() {
        if (initial_arguments.size() > 65536) throw std::runtime_error("Foundation argument snapshot exceeds 65536 entries");
        TemporaryObjects owned{process, {}, 0};
        owned.values.reserve(initial_arguments.size());
        owned.storage = process.allocate_guest(std::max<std::size_t>(8, initial_arguments.size() * 8));
        auto& memory = process.calling_engine();
        for (std::size_t index = 0; index < initial_arguments.size(); ++index) {
            const auto value = cf.string(initial_arguments[index]);
            owned.values.push_back(value);
            memory.write64(owned.storage + index * 8, value);
        }
        return call("_CFArrayCreate", {0, owned.storage, initial_arguments.size(), export_address("_kCFTypeArrayCallBacks")});
    }
    std::uint64_t environment_dictionary() {
        if (initial_environment.size() > 65536) throw std::runtime_error("Foundation environment snapshot exceeds 65536 entries");
        TemporaryObjects owned{process, {}, 0};
        owned.values.reserve(initial_environment.size() * 2);
        owned.storage = process.allocate_guest(std::max<std::size_t>(16, initial_environment.size() * 16));
        auto& memory = process.calling_engine();
        std::size_t count{};
        std::set<std::string> keys;
        const auto values = owned.storage + initial_environment.size() * 8;
        for (const auto& entry : initial_environment) {
            const auto separator = entry.find('=');
            if (!separator || separator == std::string::npos) continue;
            if (!keys.insert(entry.substr(0, separator)).second) continue; // Match Darwin getenv's first entry.
            const auto key = cf.string(entry.substr(0, separator));
            owned.values.push_back(key);
            const auto value = cf.string(entry.substr(separator + 1));
            owned.values.push_back(value);
            memory.write64(owned.storage + count * 8, key);
            memory.write64(values + count * 8, value);
            ++count;
        }
        return call("_CFDictionaryCreate", {0, owned.storage, values, count,
            export_address("_kCFTypeDictionaryKeyCallBacks"), export_address("_kCFTypeDictionaryValueCallBacks")});
    }
    void install_process_info(std::uint64_t root) {
        const auto cls = native_class("NSProcessInfo", root);
        const auto singleton = process.objects().create(runtime_call("_class_getInstanceSize", {cls}), cls,
            ObjectKind::instance, {}, {}, true);
        method(cls, "processInfo", [singleton](auto& state, auto&) { state.x[0] = singleton; }, true);
        method(cls, "processName", [this](auto& state, auto&) { state.x[0] = autorelease(cf.string(process_name)); });
        method(cls, "processIdentifier", [this](auto& state, auto&) {
            state.x[0] = runtime_call("_getpid", {}, "/usr/lib/libSystem.B.dylib");
        });
        method(cls, "arguments", [this](auto& state, auto&) { state.x[0] = autorelease(arguments_array()); });
        method(cls, "environment", [this](auto& state, auto&) { state.x[0] = autorelease(environment_dictionary()); });
        for (const auto name : {"processorCount", "activeProcessorCount"}) method(cls, name, [this, name](auto& state, auto&) {
            state.x[0] = runtime_call("_sysconf", {std::string_view(name) == "processorCount" ? 57ULL : 58ULL}, "/usr/lib/libSystem.B.dylib");
        });
        method(cls, "operatingSystemVersionString", [this](auto& state, auto&) {
            state.x[0] = autorelease(cf.string(macos_version_string));
        });
        method(cls, "operatingSystemVersion", [](auto& state, auto& memory) {
            validate_output(memory, state.x[8], 24);
            memory.write64(state.x[8], static_cast<std::uint64_t>(macos_major));
            memory.write64(add(state.x[8], 8), static_cast<std::uint64_t>(macos_minor));
            memory.write64(add(state.x[8], 16), static_cast<std::uint64_t>(macos_patch));
        });
        method(cls, "isOperatingSystemAtLeastVersion:", [](auto& state, auto& memory) {
            const auto major = std::bit_cast<std::int64_t>(memory.read64(state.x[2]));
            const auto minor = std::bit_cast<std::int64_t>(memory.read64(add(state.x[2], 8)));
            const auto patch = std::bit_cast<std::int64_t>(memory.read64(add(state.x[2], 16)));
            if (major < 0 || minor < 0 || patch < 0) throw std::runtime_error("Foundation OS version components must be nonnegative");
            state.x[0] = std::tuple{macos_major, macos_minor, macos_patch} >= std::tuple{major, minor, patch};
        });
    }
    static void validate_output(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t bytes) {
        const auto end = add(address, bytes);
        while (address < end) {
            const auto region = memory.query_region(address);
            if (!region || !region->writable || address < region->base || address - region->base >= region->size)
                throw std::runtime_error("Foundation output buffer is not writable guest memory");
            const auto remaining = region->size - (address - region->base);
            address += std::min(end - address, remaining);
        }
    }
    static void validate_format(std::string_view text) {
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] != '%') continue;
            if (++index == text.size()) throw std::runtime_error("Foundation NSLog has an incomplete conversion");
            if (text[index] == '%') continue;
            unsigned wide{};
            while (text[index] == 'l' && wide < 2) {
                ++wide;
                if (++index == text.size()) throw std::runtime_error("Foundation NSLog has an incomplete conversion");
            }
            const auto conversion = text[index];
            if ((wide && conversion != 'd') || (conversion != 'd' && conversion != 's' && conversion != '@'))
                throw std::runtime_error("Foundation NSLog unsupported conversion %" + std::string(wide, 'l') + conversion);
        }
    }
    void log(std::uint64_t format, std::uint64_t arguments, transmute::GuestMemory& memory) {
        const auto text = cf.utf8(format);
        if (text.size() > 65536) throw std::runtime_error("Foundation NSLog format exceeds 65536 bytes");
        validate_format(text);
        std::string output;
        const auto append = [&output](std::string_view value) {
            if (value.size() > 1048576 - output.size()) throw std::runtime_error("Foundation NSLog output exceeds 1048576 bytes");
            output.append(value);
        };
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] != '%') { append(std::string_view(text).substr(index, 1)); continue; }
            if (++index == text.size()) throw std::runtime_error("Foundation NSLog has an incomplete conversion");
            if (text[index] == '%') { append("%"); continue; }
            unsigned wide{};
            while (text[index] == 'l' && wide < 2) {
                ++wide;
                if (++index == text.size()) throw std::runtime_error("Foundation NSLog has an incomplete conversion");
            }
            const auto conversion = text[index];
            if ((wide && conversion != 'd') || (conversion != 'd' && conversion != 's' && conversion != '@'))
                throw std::runtime_error("Foundation NSLog unsupported conversion %" + std::string(wide, 'l') + conversion);
            const auto value = wide || conversion != 'd' ? memory.read64(arguments) : memory.read32(arguments);
            arguments = add(arguments, 8); // Apple ARM64 variadic values occupy aligned stack slots.
            if (conversion == 'd') {
                const auto number = wide ? std::bit_cast<std::int64_t>(value) :
                    static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)));
                std::array<char, 32> buffer{};
                const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
                if (error != std::errc{}) throw std::runtime_error("Foundation NSLog integer conversion failed");
                append({buffer.data(), static_cast<std::size_t>(end - buffer.data())});
            } else if (!value) append("(null)");
            else if (conversion == 's') {
                bool terminated{};
                const auto bytes = memory.read_string(value, 1, 65536, &terminated);
                if (!terminated) throw std::runtime_error("Foundation NSLog guest string is unterminated");
                append({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            } else {
                const auto description = runtime_call("_objc_msgSend", {value, objc.selector("description")});
                append(description ? cf.utf8(description) : "(null)");
            }
        }
        if (output.empty() || output.back() != '\n') append("\n");
        std::size_t written{};
        while (written < output.size()) {
            const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(output.data() + written), output.size() - written);
            const auto result = process.output(2, bytes);
            if (result.bytes_written > bytes.size()) throw std::runtime_error("Foundation NSLog output backend returned an invalid count");
            written += result.bytes_written;
            if (result.error || !result.bytes_written) {
                process.set_errno(result.error ? result.error : 5);
                throw std::runtime_error("Foundation NSLog output error " + std::to_string(result.error ? result.error : 5));
            }
        }
    }
    std::uint64_t make_class(std::string name, std::uint64_t root, ObjectKind kind, std::uint64_t storage = 0) {
        const auto cls = storage ? objc.register_native_class_in(storage, name, root) : objc.register_native_class(name, root);
        cf.set_class(kind, cls);
        objc.publish_class(name, cls, foundation_library);
        return cls;
    }
    void method(std::uint64_t cls, std::string name, transmute::GuestCallHandler handler, bool class_method = false) {
        // Encodings describe the guest ARM64 ABI, independently of Windows C types.
        static constexpr std::pair<std::string_view, std::string_view> encodings[] = {
            {"stringWithUTF8String:", "@@:*"}, {"initWithUTF8String:", "@@:*"},
            {"stringWithString:", "@@:@"}, {"initWithString:", "@@:@"},
            {"initWithCharacters:length:", "@@:^SQ"}, {"length", "Q@:"},
            {"UTF8String", "*@:"}, {"characterAtIndex:", "S@:Q"}, {"compare:", "q@:@"},
            {"isEqualToString:", "B@:@"}, {"stringByAppendingString:", "@@:@"},
            {"dataWithBytes:length:", "@@:^vQ"}, {"initWithBytes:length:", "@@:^vQ"},
            {"bytes", "^v@:"}, {"isEqualToData:", "B@:@"},
            {"numberWithInt:", "@@:i"}, {"initWithInt:", "@@:i"},
            {"numberWithLongLong:", "@@:q"}, {"initWithLongLong:", "@@:q"},
            {"numberWithDouble:", "@@:d"}, {"initWithDouble:", "@@:d"},
            {"numberWithBool:", "@@:B"}, {"initWithBool:", "@@:B"},
            {"longLongValue", "q@:"}, {"intValue", "i@:"}, {"doubleValue", "d@:"}, {"boolValue", "B@:"},
            {"isEqualToNumber:", "B@:@"},
            {"count", "Q@:"}, {"objectAtIndex:", "@@:Q"}, {"objectAtIndexedSubscript:", "@@:Q"},
            {"objectForKey:", "@@:@"}, {"objectForKeyedSubscript:", "@@:@"},
            {"isEqual:", "B@:@"}, {"hash", "Q@:"}, {"copyWithZone:", "@@:^v"},
            {"drain", "v@:"}, {"release", "v@:"}, {"dealloc", "v@:"},
            {"processIdentifier", "i@:"}, {"processorCount", "Q@:"}, {"activeProcessorCount", "Q@:"},
            {"operatingSystemVersion", "{_NSOperatingSystemVersion=qqq}@:"},
            {"isOperatingSystemAtLeastVersion:", "B@:{_NSOperatingSystemVersion=qqq}"}
        };
        std::string types = "@@:";
        for (const auto& [selector, encoding] : encodings)
            if (name == selector) { types = encoding; break; }
        static_cast<void>(objc.add_native_method(cls, std::move(name), std::move(handler), class_method, std::move(types)));
    }
    void foundation_function(std::string symbol, transmute::GuestCallHandler handler) {
        static_cast<void>(process.install_function(std::move(symbol), std::move(handler), foundation_library));
    }
    std::uint64_t export_address(std::string_view symbol) const {
        const auto value = process.resolve(core_foundation_library, symbol);
        if (!value) throw std::runtime_error("Foundation bridge: missing CoreFoundation service " + std::string(symbol));
        return value->address;
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        return cf.invoke(name, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t autorelease(std::uint64_t object) {
        const auto exported = process.resolve(objc_library, "_objc_autorelease");
        if (!exported) throw std::runtime_error("Foundation bridge requires objc_autorelease");
        const std::array arguments{object};
        try { return process.calling_engine().invoke_callback(exported->address, arguments); }
        catch (...) { process.objects().release(object); throw; }
    }
    template<class Create> void constructor(std::uint64_t cls, std::string factory, std::string initialize, Create create) {
        method(cls, std::move(factory), [this, create](auto& state, auto& memory) {
            state.x[0] = autorelease(create(state, memory));
        }, true);
        method(cls, std::move(initialize), [this, create](auto& state, auto& memory) {
            const auto value = create(state, memory);
            replace_initialized(state, value);
        });
    }
    void replace_initialized(transmute::CpuState& state, std::uint64_t value) {
        try { process.objects().release(state.x[0]); }
        catch (...) { process.objects().release(value); throw; }
        state.x[0] = value;
    }
    std::uint64_t utf8(std::uint64_t object) {
        return call("_CFStringGetCStringPtr", {object, 0x08000100});
    }
    void numeric_constructor(std::uint64_t cls, std::string factory, std::string initialize,
                             std::uint64_t type, bool floating) {
        constructor(cls, std::move(factory), std::move(initialize), [this, type, floating](auto& state, auto& memory) {
            const auto scratch = process.allocate_guest(8);
            try {
                memory.write64(scratch, floating ? state.v[0][0] : state.x[2]);
                const auto result = call("_CFNumberCreate", {0, type, scratch});
                process.release_guest(scratch);
                return result;
            } catch (...) { process.release_guest(scratch); throw; }
        });
    }
    void number_value(transmute::CpuState& state, transmute::GuestMemory& memory,
                      std::uint64_t type, bool floating) {
        if (process.objects().require(state.x[0])->kind == ObjectKind::boolean) {
            const auto value = call("_CFBooleanGetValue", {state.x[0]});
            if (floating) { state.v[0][0] = std::bit_cast<std::uint64_t>(static_cast<double>(value)); state.v[0][1] = 0; }
            else state.x[0] = value;
            return;
        }
        const auto scratch = process.allocate_guest(8);
        try {
            static_cast<void>(call("_CFNumberGetValue", {state.x[0], type, scratch}));
            const auto bits = type == 3 ? memory.read32(scratch) : memory.read64(scratch);
            process.release_guest(scratch);
            if (floating) { state.v[0][0] = bits; state.v[0][1] = 0; }
            else state.x[0] = bits;
        } catch (...) { process.release_guest(scratch); throw; }
    }
    std::uint64_t name_string(std::string_view symbol, std::uint64_t argument, transmute::GuestMemory& memory) {
        const auto service = process.resolve(objc_library, symbol);
        if (!service) throw std::runtime_error("Foundation bridge: missing Objective-C name service");
        const std::array arguments{argument};
        const auto pointer = process.calling_engine().invoke_callback(service->address, arguments);
        bool terminated{};
        const auto bytes = memory.read_string(pointer, 1, 65536, &terminated);
        if (!terminated) throw std::runtime_error("Foundation bridge: unterminated runtime name");
        return autorelease(cf.string({reinterpret_cast<const char*>(bytes.data()), bytes.size()}));
    }
};

FoundationBridge::FoundationBridge(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf)
    : impl_(std::make_unique<Impl>(process, objc, cf)) {}
FoundationBridge::~FoundationBridge() = default;
} // namespace alchemy
