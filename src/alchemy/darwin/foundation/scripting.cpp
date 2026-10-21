#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/foundation/sdef.hpp"

#include <map>

// Cocoa scripting (Foundation) and NSAppleScript.
//
// Scripting definitions come from the main bundle's OSAScriptingDefinition
// (.sdef, sdef.cpp). NSScriptClassDescription describes its classes
// (+classDescriptionForClass: walks the class and its superclasses by Cocoa
// class name; keys, types, to-one/to-many relationships, codes);
// -[NSObject classDescription] uses it.
//
// Object specifiers: NSScriptObjectSpecifier (container class description,
// container specifier, key, -keyClassDescription, -objectsByEvaluatingSpecifier:
// the container specifier's objects, or the application when it is nil, then
// -objectsByEvaluatingWithContainers:), NSPropertySpecifier (the key's
// value), NSUniqueIDSpecifier (-valueWithUniqueID:inPropertyWithKey:),
// NSIndexSpecifier and NSNameSpecifier, with NSObject's
// NSScriptKeyValueCoding lookups (-valueAtIndex:inPropertyWithKey:,
// -valueWithName:inPropertyWithKey:, -valueWithUniqueID:inPropertyWithKey:
// over the KVC collection, matching -name / -uniqueID).
//
// Commands: NSScriptCommandDescription and NSScriptCommand (arguments,
// receivers specifier, evaluated arguments and receivers, script errors,
// -executeCommand runs -performDefaultImplementation with the command as
// +currentCommand of its thread) with the standard subclasses (NSDeleteCommand,
// NSMoveCommand, ...). No Apple events reach an Alchemy process, so commands
// exist only when the application creates them; -appleEvent is nil and
// suspending a command fails explicitly.
//
// NSAppleScript keeps its source; there is no AppleScript compiler or
// interpreter, so compiling and executing fail explicitly (an uncompiled
// script has no rich text source, as documented). NSAppleEventDescriptor is
// a value container: typed values, lists, records and keyword parameters.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct ClassDescriptionState final : ObjectPayload { std::string name; };
struct CommandDescriptionState final : ObjectPayload { std::string name, suite, code, cocoa_class; };
struct SpecifierState final : ObjectPayload {
    std::mutex mutex;
    Address container_description{}, container_specifier{}, key{}, value{};   // +1; value: unique ID / name
    std::int64_t index{};
    std::int64_t error{};
};
struct CommandState final : ObjectPayload {
    std::mutex mutex;
    Address description{}, direct{}, receivers{}, arguments{}, error_string{};   // +1
    std::int64_t error_number{};
};
struct ScriptState final : ObjectPayload { Address source{}; };   // +1
struct DescriptorState final : ObjectPayload {
    std::mutex mutex;
    std::uint32_t type{};             // DescType
    std::vector<std::uint8_t> data;
    std::vector<Address> items;       // lists (+1)
    std::map<std::uint32_t, Address> keyed;   // records and event parameters (+1)
};

std::uint32_t code_of(std::string_view text) { return sdef::four_char_code(text); }

struct Scripting {
    explicit Scripting(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::optional<sdef::Definition> definition;
    bool loaded{};
    std::map<std::string, Address> class_descriptions;   // sdef class name -> object (kept)
    Address class_description_class{}, command_description_class{}, descriptor_class{};

    const sdef::Definition& defs() {
        static const sdef::Definition empty;
        {
            std::lock_guard lock(mutex);
            if (loaded) return definition ? *definition : empty;
        }
        // Read outside the lock: the bundle and data messages run guest code.
        std::optional<sdef::Definition> parsed;
        const auto bundle = support.send_id(class_named(support, "NSBundle"), "mainBundle");
        const auto name = support.send_id(bundle, "objectForInfoDictionaryKey:", {string(support, "OSAScriptingDefinition")});
        if (name) {
            const auto path = support.send_id(bundle, "pathForResource:ofType:", {name, 0});
            const auto data = path ? support.send_id(class_named(support, "NSData"), "dataWithContentsOfFile:", {path}) : 0;
            if (data) {
                std::string xml(support.send_id(data, "length"), '\0');
                if (!xml.empty()) support.memory().read_bytes(support.send_id(data, "bytes"), std::span(reinterpret_cast<std::uint8_t*>(xml.data()), xml.size()));
                parsed = sdef::parse(xml);
                if (!parsed) fail("the scripting definition " + text(support, name) + " is not well-formed XML");
            }
        }
        std::lock_guard lock(mutex);
        if (!loaded) { definition = std::move(parsed); loaded = true; }
        return definition ? *definition : empty;
    }
    // The class description object (kept for the process) of an sdef class.
    Address description_named(const std::string& name) {
        const auto& d = defs();
        if (!d.by_name(name)) return 0;
        std::lock_guard lock(mutex);
        auto& slot = class_descriptions[name];
        if (!slot) {
            auto state = std::make_shared<ClassDescriptionState>();
            state->name = name;
            slot = support.create(class_description_class, state);
        }
        return slot;
    }
    const sdef::Class* class_of(Address description) {
        const auto state = description ? support.objc.find_host_state<ClassDescriptionState>(description) : nullptr;
        if (!state) return nullptr;
        return defs().by_name(state->name);
    }
    Address description_for_class(Address cls) {
        const auto& d = defs();
        for (auto current = cls; current; current = support.send_id(current, "superclass")) {
            const auto name = support.cf.utf8(support.call(foundation_library, "_NSStringFromClass", {current}));
            if (const auto* found = d.by_cocoa_class(name)) return description_named(found->name);
        }
        return 0;
    }
    // A property or element of a class (or its superclasses).
    struct Key { const sdef::Property* property{}; const sdef::Element* element{}; };
    Key key_of(const sdef::Class* value, const std::string& key) {
        for (int depth = 0; value && depth < 32; ++depth) {
            for (const auto& item : value->properties) if (item.key == key) return {&item, nullptr};
            for (const auto& item : value->elements) if (item.key == key) return {nullptr, &item};
            value = value->inherits.empty() ? nullptr : defs().by_name(value->inherits);
        }
        return {};
    }
    std::vector<std::string> keys(const sdef::Class* value, int kind) {   // 0 attributes, 1 to-one, 2 to-many
        std::vector<std::string> result;
        for (int depth = 0; value && depth < 32; ++depth) {
            if (kind == 2) for (const auto& item : value->elements) result.push_back(item.key);
            else for (const auto& item : value->properties) {
                const bool object = defs().by_name(item.type) != nullptr;
                if ((kind == 1) == object) result.push_back(item.key);
            }
            value = value->inherits.empty() ? nullptr : defs().by_name(value->inherits);
        }
        return result;
    }
    Address strings(const std::vector<std::string>& values) {
        std::vector<Address> objects;
        for (const auto& value : values) objects.push_back(string(support, value));
        return array(support, objects);
    }

    // ---- specifiers ----
    std::shared_ptr<SpecifierState> specifier_of(Address object) { return support.objc.host_state<SpecifierState>(object); }
    // The objects a collection key holds (a value that is not an array: itself).
    std::vector<Address> elements(Address container, Address key) {
        const auto value = support.send_id(container, "valueForKey:", {key});
        if (!value) return {};
        if (!support.objc.responds(value, support.objc.selector("objectAtIndex:"))) return {value};
        std::vector<Address> result;
        const auto count = support.send_id(value, "count");
        for (Address index = 0; index < count; ++index) {
            CallRegs r;
            r.x[2] = index;
            result.push_back(support.send(value, "objectAtIndex:", r).x0);
        }
        return result;
    }
    bool equal(Address a, Address b) {
        if (a == b) return true;
        if (!a || !b) return false;
        CallRegs r;
        r.x[2] = b;
        return (support.send(a, "isEqual:", r).x0 & 0xff) != 0;
    }
    // Containers to evaluate in: the container specifier's objects, or the application.
    std::vector<Address> containers(Address specifier) {
        const auto state = specifier_of(specifier);
        Address container_specifier;
        { std::lock_guard lock(state->mutex); container_specifier = state->container_specifier; }
        if (!container_specifier) {
            const auto app_class = support.objc.find_class("NSApplication");
            if (!app_class || !support.objc.responds(app_class, support.objc.selector("sharedApplication"))) return {};
            return {support.send_id(app_class, "sharedApplication")};
        }
        const auto value = support.send_id(container_specifier, "objectsByEvaluatingSpecifier");
        if (!value) return {};
        if (support.objc.responds(value, support.objc.selector("objectAtIndex:"))) {
            std::vector<Address> result;
            const auto count = support.send_id(value, "count");
            for (Address index = 0; index < count; ++index) {
                CallRegs r;
                r.x[2] = index;
                result.push_back(support.send(value, "objectAtIndex:", r).x0);
            }
            return result;
        }
        return {value};
    }

    // ---- descriptors ----
    std::shared_ptr<DescriptorState> descriptor_of(Address object) { return support.objc.host_state<DescriptorState>(object); }
    Address make_descriptor(std::uint32_t type, std::vector<std::uint8_t> data) {
        auto state = std::make_shared<DescriptorState>();
        state->type = type;
        state->data = std::move(data);
        return autoreleased(support, make_object(support, descriptor_class, state));
    }
};

std::shared_ptr<Scripting> scripting_instance;
thread_local std::vector<Address> current_commands;

} // namespace

void install_scripting(Support& support) {
    auto self = std::make_shared<Scripting>(support);
    scripting_instance = self;
    auto class_description = support.objc.find_class("NSClassDescription");
    if (!class_description) class_description = support.declare_class("NSClassDescription", "NSObject", foundation_library);
    const auto scd = self->class_description_class = support.declare_class("NSScriptClassDescription", "NSClassDescription", foundation_library);
    const auto command_description = self->command_description_class = support.declare_class("NSScriptCommandDescription", "NSObject", foundation_library);
    const auto specifier = support.declare_class("NSScriptObjectSpecifier", "NSObject", foundation_library);
    const auto property_specifier = support.declare_class("NSPropertySpecifier", "NSScriptObjectSpecifier", foundation_library);
    const auto unique_specifier = support.declare_class("NSUniqueIDSpecifier", "NSScriptObjectSpecifier", foundation_library);
    const auto index_specifier = support.declare_class("NSIndexSpecifier", "NSScriptObjectSpecifier", foundation_library);
    const auto name_specifier = support.declare_class("NSNameSpecifier", "NSScriptObjectSpecifier", foundation_library);
    const auto command = support.declare_class("NSScriptCommand", "NSObject", foundation_library);
    const auto root = class_named(support, "NSObject");

    // ---- class descriptions ----
    for (const auto cls : {class_description, scd})
        support.method(cls, "classDescriptionForClass:", [self](CpuState& s, GuestMemory&) {
            s.x[0] = s.x[2] ? self->description_for_class(s.x[2]) : 0;
        }, "@@:#", true);
    support.method(root, "classDescription", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->description_for_class(self->support.objc.object_class(s.x[0]));
    }, "@@:");
    support.method(root, "classCode", [self](CpuState& s, GuestMemory&) {
        const auto* value = self->class_of(self->description_for_class(self->support.objc.object_class(s.x[0])));
        s.x[0] = value ? code_of(value->code) : 0;
    }, "I@:");
    const auto described = [self](Address object) {
        const auto* value = self->class_of(object);
        if (!value) fail("NSScriptClassDescription without its scripting class");
        return value;
    };
    support.method(scd, "className", [self, described](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, described(s.x[0])->name); }, "@@:");
    support.method(scd, "implementationClassName", [self, described](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, described(s.x[0])->cocoa_class); }, "@@:");
    support.method(scd, "suiteName", [self, described](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, described(s.x[0])->suite); }, "@@:");
    support.method(scd, "appleEventCode", [described](CpuState& s, GuestMemory&) { s.x[0] = code_of(described(s.x[0])->code); }, "I@:");
    support.method(scd, "superclassDescription", [self, described](CpuState& s, GuestMemory&) {
        const auto* value = described(s.x[0]);
        s.x[0] = value->inherits.empty() ? 0 : self->description_named(value->inherits);
    }, "@@:");
    support.method(scd, "attributeKeys", [self, described](CpuState& s, GuestMemory&) { s.x[0] = self->strings(self->keys(described(s.x[0]), 0)); }, "@@:");
    support.method(scd, "toOneRelationshipKeys", [self, described](CpuState& s, GuestMemory&) { s.x[0] = self->strings(self->keys(described(s.x[0]), 1)); }, "@@:");
    support.method(scd, "toManyRelationshipKeys", [self, described](CpuState& s, GuestMemory&) { s.x[0] = self->strings(self->keys(described(s.x[0]), 2)); }, "@@:");
    const auto key_of = [self, described](Address object, Address key) { return self->key_of(described(object), key ? text(self->support, key) : std::string{}); };
    support.method(scd, "typeForKey:", [self, key_of](CpuState& s, GuestMemory&) {
        const auto key = key_of(s.x[0], s.x[2]);
        s.x[0] = key.property ? string(self->support, key.property->type) : key.element ? string(self->support, key.element->type) : 0;
    }, "@@:@");
    support.method(scd, "classDescriptionForKey:", [self, key_of](CpuState& s, GuestMemory&) {
        const auto key = key_of(s.x[0], s.x[2]);
        const auto type = key.property ? key.property->type : key.element ? key.element->type : std::string{};
        s.x[0] = type.empty() ? 0 : self->description_named(type);
    }, "@@:@");
    support.method(scd, "isReadOnlyKey:", [key_of](CpuState& s, GuestMemory&) {
        const auto key = key_of(s.x[0], s.x[2]);
        s.x[0] = key.property ? key.property->access == "r" : key.element ? key.element->access == "r" : 0;
    }, "B@:@");
    support.method(scd, "hasPropertyForKey:", [key_of](CpuState& s, GuestMemory&) { s.x[0] = key_of(s.x[0], s.x[2]).property != nullptr; }, "B@:@");
    support.method(scd, "hasOrderedToManyRelationshipForKey:", [key_of](CpuState& s, GuestMemory&) { s.x[0] = key_of(s.x[0], s.x[2]).element != nullptr; }, "B@:@");
    support.method(scd, "isLocationRequiredToCreateForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");
    support.method(scd, "appleEventCodeForKey:", [key_of](CpuState& s, GuestMemory&) {
        const auto key = key_of(s.x[0], s.x[2]);
        s.x[0] = key.property ? code_of(key.property->code) : 0;
    }, "I@:@");
    support.method(scd, "keyWithAppleEventCode:", [self, described](CpuState& s, GuestMemory&) {
        const auto wanted = static_cast<std::uint32_t>(s.x[2]);
        for (const auto* value = described(s.x[0]); value; value = value->inherits.empty() ? nullptr : self->defs().by_name(value->inherits))
            for (const auto& item : value->properties) if (code_of(item.code) == wanted) { s.x[0] = string(self->support, item.key); return; }
        s.x[0] = 0;
    }, "@@:I");
    support.method(scd, "supportsCommand:", [self, described](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? text(self->support, self->support.send_id(s.x[2], "commandName")) : std::string{};
        for (const auto* value = described(s.x[0]); value; value = value->inherits.empty() ? nullptr : self->defs().by_name(value->inherits))
            for (const auto& item : value->responds_to) if (item == name) { s.x[0] = 1; return; }
        s.x[0] = 0;
    }, "B@:@");
    support.method(scd, "defaultSubcontainerAttributeKey", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");

    // ---- command descriptions ----
    const auto command_state = [self](Address object) {
        auto state = self->support.objc.find_host_state<CommandDescriptionState>(object);
        if (!state) fail("NSScriptCommandDescription is not initialized");
        return state;
    };
    support.method(command_description, "initWithSuiteName:commandName:dictionary:", [self](CpuState& s, GuestMemory&) {
        auto state = self->support.objc.host_state<CommandDescriptionState>(s.x[0]);
        state->suite = text(self->support, s.x[2]);
        state->name = text(self->support, s.x[3]);
        for (const auto& value : self->defs().commands)
            if (value.name == state->name) { state->code = value.code; state->cocoa_class = value.cocoa_class; }
    }, "@@:@@@");
    support.method(command_description, "commandName", [self, command_state](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, command_state(s.x[0])->name); }, "@@:");
    support.method(command_description, "suiteName", [self, command_state](CpuState& s, GuestMemory&) { s.x[0] = string(self->support, command_state(s.x[0])->suite); }, "@@:");
    support.method(command_description, "commandClassName", [self, command_state](CpuState& s, GuestMemory&) {
        const auto name = command_state(s.x[0])->cocoa_class;
        s.x[0] = string(self->support, name.empty() ? "NSScriptCommand" : name);
    }, "@@:");
    support.method(command_description, "appleEventCode", [command_state](CpuState& s, GuestMemory&) {
        const auto code = command_state(s.x[0])->code;
        s.x[0] = code.size() == 8 ? code_of(code.substr(4)) : code_of(code);
    }, "I@:");
    support.method(command_description, "appleEventClassCode", [command_state](CpuState& s, GuestMemory&) {
        const auto code = command_state(s.x[0])->code;
        s.x[0] = code.size() == 8 ? code_of(code.substr(0, 4)) : 0;
    }, "I@:");

    // ---- specifiers ----
    support.method(specifier, "initWithContainerClassDescription:containerSpecifier:key:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->specifier_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->container_description = objc_retain(self->support, s.x[2]);
        state->container_specifier = objc_retain(self->support, s.x[3]);
        state->key = s.x[4] ? self->support.send_id(s.x[4], "copy") : 0;
    }, "@@:@@@");
    support.method(specifier, "initWithContainerSpecifier:key:", [self](CpuState& s, GuestMemory&) {
        const auto description = s.x[2] ? self->support.send_id(s.x[2], "keyClassDescription") : 0;
        s.x[0] = self->support.send_id(s.x[0], "initWithContainerClassDescription:containerSpecifier:key:", {description, s.x[2], s.x[3]});
    }, "@@:@@");
    support.method(unique_specifier, "initWithContainerClassDescription:containerSpecifier:key:uniqueID:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(s.x[0], "initWithContainerClassDescription:containerSpecifier:key:", {s.x[2], s.x[3], s.x[4]});
        const auto state = self->specifier_of(object);
        std::lock_guard lock(state->mutex);
        state->value = objc_retain(self->support, s.x[5]);
        s.x[0] = object;
    }, "@@:@@@@");
    support.method(name_specifier, "initWithContainerClassDescription:containerSpecifier:key:name:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(s.x[0], "initWithContainerClassDescription:containerSpecifier:key:", {s.x[2], s.x[3], s.x[4]});
        const auto state = self->specifier_of(object);
        std::lock_guard lock(state->mutex);
        state->value = s.x[5] ? self->support.send_id(s.x[5], "copy") : 0;
        s.x[0] = object;
    }, "@@:@@@@");
    support.method(index_specifier, "initWithContainerClassDescription:containerSpecifier:key:index:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(s.x[0], "initWithContainerClassDescription:containerSpecifier:key:", {s.x[2], s.x[3], s.x[4]});
        const auto state = self->specifier_of(object);
        std::lock_guard lock(state->mutex);
        state->index = static_cast<std::int64_t>(s.x[5]);
        s.x[0] = object;
    }, "@@:@@@q");
    support.method(specifier, "dealloc", [self, specifier](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<SpecifierState>(s.x[0]))
            for (auto* slot : {&state->container_description, &state->container_specifier, &state->key, &state->value})
                objc_release(self->support, std::exchange(*slot, 0));
        static_cast<void>(self->support.objc.send_super(s.x[0], specifier, self->support.objc.selector("dealloc")));
    }, "v@:");
    const auto field = [self](Address object, Address SpecifierState::*member) {
        const auto state = self->specifier_of(object);
        std::lock_guard lock(state->mutex);
        return (*state).*member;
    };
    const auto set_field = [self](Address object, Address SpecifierState::*member, Address value, bool copy) {
        const auto state = self->specifier_of(object);
        const auto kept = value ? (copy ? self->support.send_id(value, "copy") : objc_retain(self->support, value)) : 0;
        Address old;
        { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, kept); }
        objc_release(self->support, old);
    };
    support.method(specifier, "containerClassDescription", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &SpecifierState::container_description); }, "@@:");
    support.method(specifier, "setContainerClassDescription:", [set_field](CpuState& s, GuestMemory&) { set_field(s.x[0], &SpecifierState::container_description, s.x[2], false); }, "v@:@");
    support.method(specifier, "containerSpecifier", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &SpecifierState::container_specifier); }, "@@:");
    support.method(specifier, "setContainerSpecifier:", [set_field](CpuState& s, GuestMemory&) { set_field(s.x[0], &SpecifierState::container_specifier, s.x[2], false); }, "v@:@");
    support.method(specifier, "key", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &SpecifierState::key); }, "@@:");
    support.method(specifier, "setKey:", [set_field](CpuState& s, GuestMemory&) { set_field(s.x[0], &SpecifierState::key, s.x[2], true); }, "v@:@");
    support.method(specifier, "containerIsObjectBeingTested", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(specifier, "containerIsRangeContainerObject", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(specifier, "evaluationErrorNumber", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(self->specifier_of(s.x[0])->error); }, "q@:");
    support.method(specifier, "keyClassDescription", [self, field](CpuState& s, GuestMemory&) {
        const auto container = field(s.x[0], &SpecifierState::container_description), key = field(s.x[0], &SpecifierState::key);
        s.x[0] = container && key ? self->support.send_id(container, "classDescriptionForKey:", {key}) : 0;
    }, "@@:");
    support.method(unique_specifier, "uniqueID", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &SpecifierState::value); }, "@@:");
    support.method(unique_specifier, "setUniqueID:", [set_field](CpuState& s, GuestMemory&) { set_field(s.x[0], &SpecifierState::value, s.x[2], false); }, "v@:@");
    support.method(name_specifier, "name", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &SpecifierState::value); }, "@@:");
    support.method(name_specifier, "setName:", [set_field](CpuState& s, GuestMemory&) { set_field(s.x[0], &SpecifierState::value, s.x[2], true); }, "v@:@");
    support.method(index_specifier, "index", [self](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(self->specifier_of(s.x[0])->index); }, "q@:");
    support.method(index_specifier, "setIndex:", [self](CpuState& s, GuestMemory&) { self->specifier_of(s.x[0])->index = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // Evaluation: the objects in every container; one object stays unwrapped.
    support.method(specifier, "objectsByEvaluatingSpecifier", [self](CpuState& s, GuestMemory&) {
        const auto containers = self->containers(s.x[0]);
        if (containers.empty()) { self->specifier_of(s.x[0])->error = 1; s.x[0] = 0; return; }   // NSNoSpecifierError
        if (containers.size() == 1) { s.x[0] = self->support.send_id(s.x[0], "objectsByEvaluatingWithContainers:", {containers.front()}); return; }
        s.x[0] = self->support.send_id(s.x[0], "objectsByEvaluatingWithContainers:", {array(self->support, containers)});
    }, "@@:");
    support.method(specifier, "objectsByEvaluatingWithContainers:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSScriptObjectSpecifier objectsByEvaluatingWithContainers:] of an abstract specifier");
    }, "@@:@");
    const auto per_container = [self](Address specifier_object, Address containers_object, auto&& one) -> Address {
        if (containers_object && self->support.objc.responds(containers_object, self->support.objc.selector("objectAtIndex:"))) {
            std::vector<Address> results;
            const auto count = self->support.send_id(containers_object, "count");
            for (Address index = 0; index < count; ++index) {
                CallRegs r;
                r.x[2] = index;
                if (const auto value = one(specifier_object, self->support.send(containers_object, "objectAtIndex:", r).x0)) results.push_back(value);
            }
            return array(self->support, results);
        }
        return one(specifier_object, containers_object);
    };
    support.method(property_specifier, "objectsByEvaluatingWithContainers:", [self, per_container, field](CpuState& s, GuestMemory&) {
        s.x[0] = per_container(s.x[0], s.x[2], [self, field](Address spec, Address container) {
            return container ? self->support.send_id(container, "valueForKey:", {field(spec, &SpecifierState::key)}) : 0;
        });
    }, "@@:@");
    support.method(unique_specifier, "objectsByEvaluatingWithContainers:", [self, per_container, field](CpuState& s, GuestMemory&) {
        s.x[0] = per_container(s.x[0], s.x[2], [self, field](Address spec, Address container) {
            return container ? self->support.send_id(container, "valueWithUniqueID:inPropertyWithKey:",
                                                     {field(spec, &SpecifierState::value), field(spec, &SpecifierState::key)}) : 0;
        });
    }, "@@:@");
    support.method(name_specifier, "objectsByEvaluatingWithContainers:", [self, per_container, field](CpuState& s, GuestMemory&) {
        s.x[0] = per_container(s.x[0], s.x[2], [self, field](Address spec, Address container) {
            return container ? self->support.send_id(container, "valueWithName:inPropertyWithKey:",
                                                     {field(spec, &SpecifierState::value), field(spec, &SpecifierState::key)}) : 0;
        });
    }, "@@:@");
    support.method(index_specifier, "objectsByEvaluatingWithContainers:", [self, per_container, field](CpuState& s, GuestMemory&) {
        s.x[0] = per_container(s.x[0], s.x[2], [self, field](Address spec, Address container) -> Address {
            if (!container) return 0;
            const auto key = field(spec, &SpecifierState::key);
            auto index = self->specifier_of(spec)->index;
            if (index < 0) index += static_cast<std::int64_t>(self->elements(container, key).size());   // negative: from the end
            if (index < 0) return 0;
            return self->support.send_id(container, "valueAtIndex:inPropertyWithKey:", {static_cast<Address>(index), key});
        });
    }, "@@:@");
    // NSScriptKeyValueCoding lookups over the KVC collection.
    support.method(root, "valueAtIndex:inPropertyWithKey:", [self](CpuState& s, GuestMemory&) {
        const auto list = self->elements(s.x[0], s.x[3]);
        s.x[0] = s.x[2] < list.size() ? list[s.x[2]] : 0;
    }, "@@:Q@");
    support.method(root, "valueWithName:inPropertyWithKey:", [self](CpuState& s, GuestMemory&) {
        for (const auto value : self->elements(s.x[0], s.x[3]))
            if (self->support.objc.responds(value, self->support.objc.selector("name")) && self->equal(self->support.send_id(value, "name"), s.x[2])) { s.x[0] = value; return; }
        s.x[0] = 0;
    }, "@@:@@");
    support.method(root, "valueWithUniqueID:inPropertyWithKey:", [self](CpuState& s, GuestMemory&) {
        for (const auto value : self->elements(s.x[0], s.x[3]))
            if (self->support.objc.responds(value, self->support.objc.selector("uniqueID")) && self->equal(self->support.send_id(value, "uniqueID"), s.x[2])) { s.x[0] = value; return; }
        s.x[0] = 0;
    }, "@@:@@");

    // ---- commands ----
    const auto command_of = [self](Address object) { return self->support.objc.host_state<CommandState>(object); };
    support.method(command, "initWithCommandDescription:", [self, command_of](CpuState& s, GuestMemory&) {
        command_of(s.x[0])->description = objc_retain(self->support, s.x[2]);
    }, "@@:@");
    support.method(command, "init", [command_of](CpuState& s, GuestMemory&) { static_cast<void>(command_of(s.x[0])); }, "@@:");
    support.method(command, "dealloc", [self, command](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<CommandState>(s.x[0]))
            for (auto* slot : {&state->description, &state->direct, &state->receivers, &state->arguments, &state->error_string})
                objc_release(self->support, std::exchange(*slot, 0));
        static_cast<void>(self->support.objc.send_super(s.x[0], command, self->support.objc.selector("dealloc")));
    }, "v@:");
    struct Slot { const char* getter; const char* setter; Address CommandState::*member; bool copy; };
    for (const auto& slot : {Slot{"commandDescription", nullptr, &CommandState::description, false},
                             Slot{"directParameter", "setDirectParameter:", &CommandState::direct, false},
                             Slot{"receiversSpecifier", "setReceiversSpecifier:", &CommandState::receivers, false},
                             Slot{"arguments", "setArguments:", &CommandState::arguments, true},
                             Slot{"scriptErrorString", "setScriptErrorString:", &CommandState::error_string, true}}) {
        support.method(command, slot.getter, [command_of, member = slot.member](CpuState& s, GuestMemory&) {
            const auto state = command_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = (*state).*member;
        }, "@@:");
        if (slot.setter)
            support.method(command, slot.setter, [self, command_of, member = slot.member, copy = slot.copy](CpuState& s, GuestMemory&) {
                const auto state = command_of(s.x[0]);
                const auto kept = s.x[2] ? (copy ? self->support.send_id(s.x[2], "copy") : objc_retain(self->support, s.x[2])) : 0;
                Address old;
                { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, kept); }
                objc_release(self->support, old);
            }, "v@:@");
    }
    support.method(command, "scriptErrorNumber", [command_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(command_of(s.x[0])->error_number); }, "q@:");
    support.method(command, "setScriptErrorNumber:", [command_of](CpuState& s, GuestMemory&) { command_of(s.x[0])->error_number = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    support.method(command, "isWellFormed", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(command, "appleEvent", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // commands are never made from Apple events here
    support.method(command, "currentCommand", [](CpuState& s, GuestMemory&) { s.x[0] = current_commands.empty() ? 0 : current_commands.back(); }, "@@:", true);
    // Specifier values are replaced by the objects they evaluate to.
    const auto evaluate = [self](Address value) -> Address {
        if (value && self->support.send(value, "isKindOfClass:", [&] { CallRegs r; r.x[2] = class_named(self->support, "NSScriptObjectSpecifier"); return r; }()).x0 & 0xff)
            return self->support.send_id(value, "objectsByEvaluatingSpecifier");
        return value;
    };
    support.method(command, "evaluatedReceivers", [command_of, evaluate](CpuState& s, GuestMemory&) {
        const auto state = command_of(s.x[0]);
        Address receivers;
        { std::lock_guard lock(state->mutex); receivers = state->receivers; }
        s.x[0] = receivers ? evaluate(receivers) : 0;
    }, "@@:");
    support.method(command, "evaluatedArguments", [self, command_of, evaluate](CpuState& s, GuestMemory&) {
        const auto state = command_of(s.x[0]);
        Address arguments;
        { std::lock_guard lock(state->mutex); arguments = state->arguments; }
        if (!arguments) { s.x[0] = 0; return; }
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [key, value] : self->support.cf.dictionary_entries(arguments)) {
            const auto evaluated = evaluate(value);
            if (!evaluated) { s.x[0] = 0; return; }   // an argument that does not evaluate makes the command ill-formed: nil
            entries.emplace_back(key, evaluated);
        }
        s.x[0] = autoreleased(self->support, self->support.cf.dictionary(entries));
    }, "@@:");
    support.method(command, "performDefaultImplementation", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // subclasses override
    support.method(command, "executeCommand", [self](CpuState& s, GuestMemory&) {
        current_commands.push_back(s.x[0]);
        try { s.x[0] = self->support.send_id(s.x[0], "performDefaultImplementation"); }
        catch (...) { current_commands.pop_back(); throw; }
        current_commands.pop_back();
    }, "@@:");
    support.method(command, "suspendExecution", [](CpuState&, GuestMemory&) { unsupported("-[NSScriptCommand suspendExecution]"); }, "v@:");
    support.method(command, "resumeExecutionWithResult:", [](CpuState&, GuestMemory&) { unsupported("-[NSScriptCommand resumeExecutionWithResult:]"); }, "v@:@");
    for (const auto* name : {"NSCloneCommand", "NSCloseCommand", "NSCountCommand", "NSCreateCommand", "NSDeleteCommand", "NSExistsCommand",
                             "NSGetCommand", "NSMoveCommand", "NSQuitCommand", "NSSetCommand"})
        static_cast<void>(support.declare_class(name, "NSScriptCommand", foundation_library));
    for (const auto* name : {"NSDeleteCommand", "NSMoveCommand", "NSCloneCommand", "NSSetCommand"}) {
        const auto cls = class_named(support, name);
        // The receivers' specifier: the object(s) the command acts on.
        support.method(cls, "keySpecifier", [command_of](CpuState& s, GuestMemory&) {
            const auto state = command_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = state->receivers;
        }, "@@:");
    }

    // ---- NSAppleScript ----
    const auto script = support.declare_class("NSAppleScript", "NSObject", foundation_library);
    support.method(script, "initWithSource:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { self->support.release(s.x[0]); s.x[0] = 0; return; }
        self->support.objc.host_state<ScriptState>(s.x[0])->source = self->support.send_id(s.x[2], "copy");
    }, "@@:@");
    support.method(script, "initWithContentsOfURL:error:", [self](CpuState& s, GuestMemory&) {
        const auto source = s.x[2] ? self->support.send_id(class_named(self->support, "NSString"), "stringWithContentsOfURL:encoding:error:", {s.x[2], 4, s.x[3]}) : 0;
        if (!source) { self->support.release(s.x[0]); s.x[0] = 0; return; }   // compiled scripts (.scpt) are not text
        self->support.objc.host_state<ScriptState>(s.x[0])->source = objc_retain(self->support, source);
    }, "@@:@^@");
    support.method(script, "source", [self](CpuState& s, GuestMemory&) { s.x[0] = self->support.objc.host_state<ScriptState>(s.x[0])->source; }, "@@:");
    support.method(script, "isCompiled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(script, "richTextSource", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // nil until compiled
    for (const auto* name : {"compileAndReturnError:", "executeAndReturnError:"})
        support.method(script, name, [name = std::string(name)](CpuState&, GuestMemory&) {
            unsupported("-[NSAppleScript " + name + "] (no AppleScript compiler or interpreter under Alchemy)");
        }, "B@:^@");
    support.method(script, "executeAppleEvent:error:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSAppleScript executeAppleEvent:error:] (no AppleScript interpreter under Alchemy)");
    }, "@@:@^@");
    support.method(script, "dealloc", [self, script](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<ScriptState>(s.x[0])) objc_release(self->support, std::exchange(state->source, 0));
        static_cast<void>(self->support.objc.send_super(s.x[0], script, self->support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSAppleEventDescriptor ----
    auto descriptor = support.objc.find_class("NSAppleEventDescriptor");
    if (!descriptor) descriptor = support.declare_class("NSAppleEventDescriptor", "NSObject", foundation_library);
    self->descriptor_class = descriptor;
    constexpr std::uint32_t type_null = 0x6e756c6c, type_utf8 = 0x75746638, type_true = 0x74727565, type_false = 0x66616c73,
                            type_int32 = 0x6c6f6e67, type_list = 0x6c697374, type_record = 0x7265636f, type_type = 0x74797065, type_enum = 0x656e756d;
    support.method(descriptor, "nullDescriptor", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_descriptor(type_null, {}); }, "@@:", true);
    support.method(descriptor, "listDescriptor", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_descriptor(type_list, {}); }, "@@:", true);
    support.method(descriptor, "recordDescriptor", [self](CpuState& s, GuestMemory&) { s.x[0] = self->make_descriptor(type_record, {}); }, "@@:", true);
    support.method(descriptor, "descriptorWithString:", [self](CpuState& s, GuestMemory&) {
        const auto value = text(self->support, s.x[2]);
        s.x[0] = self->make_descriptor(type_utf8, std::vector<std::uint8_t>(value.begin(), value.end()));
    }, "@@:@", true);
    support.method(descriptor, "descriptorWithBoolean:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->make_descriptor((s.x[2] & 0xff) ? type_true : type_false, {});
    }, "@@:C", true);
    support.method(descriptor, "descriptorWithInt32:", [self](CpuState& s, GuestMemory&) {
        const auto value = static_cast<std::uint32_t>(s.x[2]);
        s.x[0] = self->make_descriptor(type_int32, {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                                    static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)});
    }, "@@:i", true);
    for (const auto& [name, type] : std::array<std::pair<const char*, std::uint32_t>, 2>{{{"descriptorWithTypeCode:", type_type}, {"descriptorWithEnumCode:", type_enum}}})
        support.method(descriptor, name, [self, type = type](CpuState& s, GuestMemory&) {
            const auto value = static_cast<std::uint32_t>(s.x[2]);
            s.x[0] = self->make_descriptor(type, {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                                  static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)});
        }, "@@:I", true);
    const auto word = [](const DescriptorState& state) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(4, state.data.size()); ++i) value |= std::uint32_t{state.data[i]} << (8 * i);
        return value;
    };
    support.method(descriptor, "descriptorType", [self](CpuState& s, GuestMemory&) { s.x[0] = self->descriptor_of(s.x[0])->type; }, "I@:");
    support.method(descriptor, "stringValue", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        s.x[0] = state->type == type_utf8 ? string(self->support, std::string(state->data.begin(), state->data.end())) : 0;
    }, "@@:");
    support.method(descriptor, "booleanValue", [self, word](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        s.x[0] = state->type == type_true || (state->type == type_int32 && word(*state) != 0);
    }, "C@:");
    support.method(descriptor, "int32Value", [self, word](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        s.x[0] = state->type == type_int32 ? word(*state) : state->type == type_true ? 1 : 0;
    }, "i@:");
    for (const auto* name : {"typeCodeValue", "enumCodeValue"})
        support.method(descriptor, name, [self, word](CpuState& s, GuestMemory&) { s.x[0] = word(*self->descriptor_of(s.x[0])); }, "I@:");
    support.method(descriptor, "data", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        GuestBlock buffer(self->support, state->data.size(), false);
        if (!state->data.empty()) self->support.memory().write_bytes(buffer, state->data);
        s.x[0] = autoreleased(self->support, self->support.cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, state->data.size()}));
    }, "@@:");
    // Keyed parameters (events) and fields (records) share the keyword store.
    for (const auto* name : {"setParamDescriptor:forKeyword:", "setDescriptor:forKeyword:", "setAttributeDescriptor:forKeyword:"})
        support.method(descriptor, name, [self](CpuState& s, GuestMemory&) {
            const auto state = self->descriptor_of(s.x[0]);
            const auto kept = objc_retain(self->support, s.x[2]);
            Address old{};
            {
                std::lock_guard lock(state->mutex);
                auto& slot = state->keyed[static_cast<std::uint32_t>(s.x[3])];
                old = std::exchange(slot, kept);
            }
            objc_release(self->support, old);
        }, "v@:@I");
    for (const auto* name : {"paramDescriptorForKeyword:", "descriptorForKeyword:", "attributeDescriptorForKeyword:"})
        support.method(descriptor, name, [self](CpuState& s, GuestMemory&) {
            const auto state = self->descriptor_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            const auto found = state->keyed.find(static_cast<std::uint32_t>(s.x[2]));
            s.x[0] = found == state->keyed.end() ? 0 : found->second;
        }, "@@:I");
    support.method(descriptor, "removeParamDescriptorWithKeyword:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        Address old{};
        {
            std::lock_guard lock(state->mutex);
            if (const auto found = state->keyed.find(static_cast<std::uint32_t>(s.x[2])); found != state->keyed.end()) { old = found->second; state->keyed.erase(found); }
        }
        objc_release(self->support, old);
    }, "v@:I");
    support.method(descriptor, "numberOfItems", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->type == type_list ? state->items.size() : state->keyed.size();
    }, "q@:");
    // List items are numbered from 1; index 0 appends.
    support.method(descriptor, "insertDescriptor:atIndex:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        const auto kept = objc_retain(self->support, s.x[2]);
        std::lock_guard lock(state->mutex);
        const auto index = static_cast<std::int64_t>(s.x[3]);
        if (index <= 0 || static_cast<std::size_t>(index) > state->items.size()) state->items.push_back(kept);
        else state->items.insert(state->items.begin() + (index - 1), kept);
    }, "v@:@q");
    support.method(descriptor, "descriptorAtIndex:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        s.x[0] = index >= 1 && static_cast<std::size_t>(index) <= state->items.size() ? state->items[static_cast<std::size_t>(index - 1)] : 0;
    }, "@@:q");
    support.method(descriptor, "dealloc", [self, descriptor](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<DescriptorState>(s.x[0])) {
            for (const auto value : std::exchange(state->items, {})) objc_release(self->support, value);
            for (const auto& [key, value] : std::exchange(state->keyed, {})) { static_cast<void>(key); objc_release(self->support, value); }
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], descriptor, self->support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
