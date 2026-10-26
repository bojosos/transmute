#include "alchemy/darwin/foundation/kvo.hpp"

#include <map>
#include <set>

// Cocoa bindings (NSKeyValueBindingCreation on NSObject) and the bindings
// controllers NSObjectController, NSArrayController and _NSManagedProxy.
//
// -bind:toObject:withKeyPath:options: observes the key path of the observed
// object (kvo.cpp; the observed object is not retained) and sets the
// receiver's bound property from it at once and on every change. Binding
// names map to properties as AppKit's do: "value" is -objectValue (-state for
// buttons), "contentArray"/"contentObject"/"content" are a controller's
// -content, the families "enabled", "enabled2", ... (AND) and "hidden",
// "hidden2", ... (OR) combine, and any other name is the receiver's KVC key
// of that name. Options: NSValueTransformerName (NSNegateBoolean, NSIsNil,
// NSIsNotNil built in; others through NSValueTransformer), NSValueTransformer,
// NSNullPlaceholder, NSNoSelectionPlaceholder, NSMultipleValuesPlaceholder,
// NSNotApplicablePlaceholder. -unbind:, -infoForBinding: (NSObservedObject,
// NSObservedKeyPath, NSOptions), -exposedBindings, +exposeBinding:,
// -valueClassForBinding: and -optionDescriptionsForBinding: complete the
// protocol. Bindings end when the receiver is deallocated.
//
// The view-to-model direction belongs to the bound control: after a user
// edit it sends -_alchemyPropagateValue:forBinding: (an Alchemy SPI on
// NSObject), which applies the reverse transformation and sets the observed
// object's key path (Foundation's NSEditor flow does the same).
//
// Controllers: content, selection and arranged objects are KVO-compliant
// (will/didChangeValueForKey: for content, arrangedObjects, selection,
// selectionIndexes, selectedObjects, canRemove, canAdd, canInsert). -selection
// is a proxy: -valueForKey: answers the single selected object's value,
// NSNoSelectionMarker or NSMultipleValuesMarker; -setValue:forKey: sets every
// selected object; observing a key path through it observes the selected
// objects. Sorting uses -sortedArrayUsingDescriptors:, filtering
// -evaluateWithObject: of the filter predicate. Core Data (entity names,
// managed object contexts, fetching) fails explicitly; _NSManagedProxy only
// carries its decoded entity name.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
constexpr Address not_found = 0x7fffffffffffffffULL;

struct Binding {
    Address receiver{}, observed{}, path{}, options{};   // path, options: +1
    std::string name, key_path;
    std::uint64_t observation{};
};

struct ControllerState final : ObjectPayload {
    std::recursive_mutex mutex;
    Address content{}, object_class{}, selection_proxy{}, arranged{}, selection_indexes{}, sort_descriptors{}, filter_predicate{};
    Address entity_name{};
    bool editable{true}, prepares_content{}, avoids_empty_selection{true}, preserves_selection{true}, selects_inserted{true};
    bool clears_filter{}, rearranges{}, array{};
};

struct ProxyState final : ObjectPayload { Address controller{}; };   // not retained (the controller owns the proxy)
struct ManagedProxyState final : ObjectPayload { Address entity_name{}; };

struct Bindings {
    explicit Bindings(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::pair<Address, std::string>, Binding> table;
    std::map<Address, std::vector<std::string>> exposed;   // class -> exposed bindings
    std::map<std::pair<Address, std::string>, std::vector<std::uint64_t>> proxy_observations;   // (proxy, key)
    Address no_selection{}, multiple_values{}, not_applicable{};
    Address object_controller{}, array_controller{}, proxy_class{};

    // ---- values ----
    bool truth(Address value) {
        if (!value || value == null_object()) return false;
        return (support.send(value, "boolValue").x0 & 0xff) != 0;
    }
    Address null_object() { return support.send_id(class_named(support, "NSNull"), "null"); }
    Address option(Address options, std::string_view name) {
        if (!options) return 0;
        return support.send_id(options, "objectForKey:", {string(support, name)});
    }
    Address transformer(Address options, Address& name_out) {
        name_out = option(options, "NSValueTransformerName");
        if (const auto object = option(options, "NSValueTransformer")) return object;
        if (!name_out) return 0;
        const auto name = text(support, name_out);
        if (name == "NSNegateBoolean" || name == "NSIsNil" || name == "NSIsNotNil") return 0;
        const auto cls = support.objc.find_class("NSValueTransformer");
        if (!cls) unsupported("binding value transformer " + name + " (NSValueTransformer is not provided)");
        const auto object = support.send_id(cls, "valueTransformerForName:", {name_out});
        if (!object) fail("NSInvalidArgumentException: no value transformer is registered for the name " + name);
        return object;
    }
    Address forward_value(const Binding& binding, Address value) {
        Address name{};
        const auto object = transformer(binding.options, name);
        if (value == no_selection) {
            const auto placeholder = option(binding.options, "NSNoSelectionPlaceholder");
            return placeholder == null_object() ? 0 : placeholder;
        }
        if (value == multiple_values) {
            const auto placeholder = option(binding.options, "NSMultipleValuesPlaceholder");
            return placeholder == null_object() ? 0 : placeholder;
        }
        if (value == not_applicable) {
            const auto placeholder = option(binding.options, "NSNotApplicablePlaceholder");
            return placeholder == null_object() ? 0 : placeholder;
        }
        if (value == null_object()) value = 0;
        if (object) value = support.send_id(object, "transformedValue:", {value});
        else if (name) {
            const auto text_name = text(support, name);
            if (text_name == "NSNegateBoolean") value = boolean(support, !truth(value));
            else if (text_name == "NSIsNil") value = boolean(support, value == 0);
            else if (text_name == "NSIsNotNil") value = boolean(support, value != 0);
        }
        if (!value) {
            const auto placeholder = option(binding.options, "NSNullPlaceholder");
            if (placeholder && placeholder != null_object()) value = placeholder;
        }
        return value;
    }
    Address reverse_value(const Binding& binding, Address value) {
        Address name{};
        const auto object = transformer(binding.options, name);
        if (object) {
            const auto cls = support.objc.object_class(object);
            if (!(support.send(cls, "allowsReverseTransformation").x0 & 0xff))
                fail("NSInternalInconsistencyException: the binding's value transformer does not allow reverse transformation");
            return support.send_id(object, "reverseTransformedValue:", {value});
        }
        if (name) {
            const auto text_name = text(support, name);
            if (text_name == "NSNegateBoolean") return boolean(support, !truth(value));
            fail("NSInternalInconsistencyException: value transformer " + text_name + " does not allow reverse transformation");
        }
        return value;
    }

    // The receiver's KVC key for a binding name.
    std::string key_for(Address receiver, const std::string& name) {
        if (name == "value") {
            if (const auto button = support.objc.find_class("NSButton")) {
                if (support.send(receiver, "isKindOfClass:", [&] { CallRegs r; r.x[2] = button; return r; }()).x0 & 0xff) return "state";
            }
            if (support.objc.responds(receiver, support.objc.selector("setObjectValue:"))) return "objectValue";
            return "value";
        }
        if (name == "contentArray" || name == "contentObject" || name == "contentSet") return "content";
        return name;
    }
    // "enabled2" -> "enabled"; other names unchanged.
    static std::string family(const std::string& name) {
        for (const auto* base : {"enabled", "hidden"}) {
            const std::string_view b(base);
            if (name.size() >= b.size() && name.compare(0, b.size(), b) == 0 &&
                name.find_first_not_of("0123456789", b.size()) == std::string::npos)
                return std::string(b);
        }
        return name;
    }
    void apply(const std::string& name, Address receiver) {
        Binding binding;
        std::vector<Binding> members;
        const auto group = family(name);
        {
            std::lock_guard lock(mutex);
            const auto found = table.find({receiver, name});
            if (found == table.end()) return;
            binding = found->second;
            if (group != name || group == "enabled" || group == "hidden")
                for (const auto& [key, value] : table)
                    if (key.first == receiver && family(key.second) == group) members.push_back(value);
        }
        if (!members.empty()) {
            // AND of the enabled family, OR of the hidden family.
            bool result = group == "enabled";
            for (const auto& member : members) {
                const auto value = truth(forward_value(member, support.send_id(member.observed, "valueForKeyPath:", {member.path})));
                result = group == "enabled" ? (result && value) : (result || value);
            }
            static_cast<void>(support.send_id(receiver, "setValue:forKey:", {boolean(support, result), string(support, group)}));
            return;
        }
        const auto value = forward_value(binding, support.send_id(binding.observed, "valueForKeyPath:", {binding.path}));
        static_cast<void>(support.send_id(receiver, "setValue:forKey:", {value, string(support, key_for(receiver, name))}));
    }

    void unbind(Address receiver, const std::string& name) {
        Binding binding;
        {
            std::lock_guard lock(mutex);
            const auto found = table.find({receiver, name});
            if (found == table.end()) return;
            binding = found->second;
            table.erase(found);
        }
        kvo::unobserve(support, binding.observation);
        support.release(binding.path);
        support.release(binding.options);
    }
    void bind(Address receiver, const std::string& name, Address observed, Address path, Address options) {
        unbind(receiver, name);
        Binding binding;
        binding.receiver = receiver;
        binding.observed = observed;
        binding.name = name;
        binding.path = support.send_id(path, "copy");
        binding.key_path = support.cf.utf8(path);
        binding.options = options ? support.send_id(options, "copy") : 0;
        {
            std::lock_guard lock(mutex);
            table[{receiver, name}] = binding;
        }
        const auto id = kvo::observe(support, observed, binding.key_path, kvo::option_new,
                                     [this, receiver, name](Address, Address) { apply(name, receiver); });
        {
            std::lock_guard lock(mutex);
            if (const auto found = table.find({receiver, name}); found != table.end()) found->second.observation = id;
        }
        kvo::on_dealloc(support, receiver, "bindings", [this](Address gone) {
            std::vector<std::string> names;
            {
                std::lock_guard lock(mutex);
                for (const auto& [key, value] : table) { static_cast<void>(value); if (key.first == gone) names.push_back(key.second); }
            }
            for (const auto& value : names) unbind(gone, value);
        });
        apply(name, receiver);
    }
    Address info(Address receiver, const std::string& name) {
        Binding binding;
        {
            std::lock_guard lock(mutex);
            const auto found = table.find({receiver, name});
            if (found == table.end()) return 0;
            binding = found->second;
        }
        const auto options = binding.options ? binding.options : dictionary(support, {});
        return dictionary(support, {{"NSObservedObject", binding.observed}, {"NSObservedKeyPath", binding.path}, {"NSOptions", options}});
    }
    void propagate(Address receiver, const std::string& name, Address value) {
        Binding binding;
        {
            std::lock_guard lock(mutex);
            const auto found = table.find({receiver, name});
            if (found == table.end()) return;
            binding = found->second;
        }
        static_cast<void>(support.send_id(binding.observed, "setValue:forKeyPath:", {reverse_value(binding, value), binding.path}));
    }

    // ---- controllers ----
    std::shared_ptr<ControllerState> state_of(Address controller) { return support.objc.host_state<ControllerState>(controller); }
    void will(Address object, std::initializer_list<const char*> keys) {
        for (const auto* key : keys) static_cast<void>(support.send_id(object, "willChangeValueForKey:", {string(support, key)}));
    }
    void did(Address object, std::initializer_list<const char*> keys) {
        // Reverse order, so nested pairs close innermost first.
        std::vector<const char*> list(keys);
        for (auto it = list.rbegin(); it != list.rend(); ++it) static_cast<void>(support.send_id(object, "didChangeValueForKey:", {string(support, *it)}));
    }
    std::vector<Address> values_of(Address array_object) {
        std::vector<Address> values;
        if (!array_object) return values;
        const auto count = support.send_id(array_object, "count");
        for (Address index = 0; index < count; ++index) {
            CallRegs r;
            r.x[2] = index;
            values.push_back(support.send(array_object, "objectAtIndex:", r).x0);
        }
        return values;
    }
    std::vector<Address> indexes_of(Address set) {
        std::vector<Address> values;
        if (!set) return values;
        for (auto index = support.send_id(set, "firstIndex"); index != not_found; index = support.send_id(set, "indexGreaterThanIndex:", {index}))
            values.push_back(index);
        return values;
    }
    Address index_set(const std::vector<Address>& indexes) {
        const auto set = support.send_id(support.send_id(class_named(support, "NSMutableIndexSet"), "alloc"), "init");
        for (const auto index : indexes) static_cast<void>(support.send_id(set, "addIndex:", {index}));
        return set;   // +1
    }
    // The array controller's content as an array (nil content: empty).
    std::vector<Address> content_values(ControllerState& state) {
        if (!state.content) return {};
        if (support.objc.responds(state.content, support.objc.selector("objectAtIndex:"))) return values_of(state.content);
        if (support.objc.responds(state.content, support.objc.selector("allObjects"))) return values_of(support.send_id(state.content, "allObjects"));
        return {state.content};
    }
    // Recomputes arrangedObjects (filter, then sort); +1 array.
    Address arrange(Address controller, ControllerState& state) {
        std::vector<Address> values;
        for (const auto value : content_values(state)) {
            if (state.filter_predicate && !(support.send(state.filter_predicate, "evaluateWithObject:", [&] { CallRegs r; r.x[2] = value; return r; }()).x0 & 0xff))
                continue;
            values.push_back(value);
        }
        Address result = support.cf.array(values);
        if (state.sort_descriptors && support.send_id(state.sort_descriptors, "count")) {
            const auto sorted = support.send_id(result, "sortedArrayUsingDescriptors:", {state.sort_descriptors});
            support.release(result);
            result = support.send_id(sorted, "copy");
        }
        static_cast<void>(controller);
        return result;
    }
    std::vector<Address> selected(Address controller) {
        const auto state = state_of(controller);
        std::lock_guard lock(state->mutex);
        if (!state->array) return state->content ? std::vector<Address>{state->content} : std::vector<Address>{};
        const auto arranged = values_of(state->arranged);
        std::vector<Address> result;
        for (const auto index : indexes_of(state->selection_indexes)) if (index < arranged.size()) result.push_back(arranged[index]);
        return result;
    }
    // Content changed: rearranges and keeps (or resets) the selection.
    void content_changed(Address controller, Address new_content, bool select_inserted_last = false) {
        const auto state = state_of(controller);
        will(controller, {"content", "arrangedObjects", "selectionIndexes", "selection", "selectedObjects", "canRemove", "canSelectNext", "canSelectPrevious"});
        {
            std::lock_guard lock(state->mutex);
            const auto previous_selection = selected_locked(*state);
            support.release(std::exchange(state->content, new_content));
            if (state->array) {
                support.release(std::exchange(state->arranged, arrange(controller, *state)));
                const auto arranged = values_of(state->arranged);
                std::vector<Address> indexes;
                if (select_inserted_last && state->selects_inserted && !arranged.empty()) indexes.push_back(arranged.size() - 1);
                else if (state->preserves_selection) {
                    for (const auto object : previous_selection)
                        for (std::size_t i = 0; i < arranged.size(); ++i) if (arranged[i] == object) { indexes.push_back(i); break; }
                }
                if (indexes.empty() && state->avoids_empty_selection && !arranged.empty()) indexes.push_back(0);
                support.release(std::exchange(state->selection_indexes, index_set(indexes)));
            }
        }
        did(controller, {"content", "arrangedObjects", "selectionIndexes", "selection", "selectedObjects", "canRemove", "canSelectNext", "canSelectPrevious"});
    }
    std::vector<Address> selected_locked(ControllerState& state) {
        if (!state.array) return state.content ? std::vector<Address>{state.content} : std::vector<Address>{};
        const auto arranged = values_of(state.arranged);
        std::vector<Address> result;
        for (const auto index : indexes_of(state.selection_indexes)) if (index < arranged.size()) result.push_back(arranged[index]);
        return result;
    }
    void set_selection(Address controller, Address indexes) {
        const auto state = state_of(controller);
        will(controller, {"selectionIndexes", "selectionIndex", "selection", "selectedObjects", "canRemove", "canSelectNext", "canSelectPrevious"});
        {
            std::lock_guard lock(state->mutex);
            std::vector<Address> list = indexes_of(indexes);
            const auto count = values_of(state->arranged).size();
            std::erase_if(list, [count](Address index) { return index >= count; });
            if (list.empty() && state->avoids_empty_selection && count) list.push_back(0);
            support.release(std::exchange(state->selection_indexes, index_set(list)));
        }
        did(controller, {"selectionIndexes", "selectionIndex", "selection", "selectedObjects", "canRemove", "canSelectNext", "canSelectPrevious"});
    }
    // A mutable copy of the content array with `edit` applied; +1.
    template <class Edit>
    Address edited_content(Address controller, Edit&& edit) {
        const auto state = state_of(controller);
        std::vector<Address> values;
        { std::lock_guard lock(state->mutex); values = content_values(*state); }
        edit(values);
        return support.cf.array(values, true);
    }
    // The selection proxy's value for a key.
    Address selection_value(Address controller, Address key) {
        const auto objects = selected(controller);
        if (objects.empty()) return no_selection;
        Address first{};
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto value = support.send_id(objects[index], "valueForKey:", {key});
            if (index == 0) first = value;
            else if (value != first && !(value && first && (support.send(value, "isEqual:", [&] { CallRegs r; r.x[2] = first; return r; }()).x0 & 0xff)))
                return multiple_values;
        }
        return first;
    }
};

} // namespace

void install_bindings(Support& support) {
    auto self = std::make_shared<Bindings>(support);
    const auto root = class_named(support, "NSObject");

    // Selection markers (AppKit exports id constants).
    const auto marker_class = support.declare_class("NSStateMarker", "NSObject");
    for (const auto& [name, slot] : std::array<std::pair<const char*, Address*>, 3>{{
             {"NSNoSelectionMarker", &self->no_selection}, {"NSMultipleValuesMarker", &self->multiple_values},
             {"NSNotApplicableMarker", &self->not_applicable}}}) {
        const auto symbol = "_" + std::string(name);
        if (const auto existing = support.process.resolve(appkit, symbol)) { *slot = support.process.main_engine().read64(existing->address); continue; }
        *slot = support.process.objects().create(16, marker_class, ObjectKind::instance, {}, {}, true);
        const auto cell = support.process.allocate_guest(8);
        support.process.main_engine().write64(cell, *slot);
        support.process.install_export(std::string(appkit), symbol, {cell, ExportKind::data});
    }
    for (const auto* name : {"NSObservedObjectKey", "NSObservedKeyPathKey", "NSOptionsKey", "NSValueTransformerNameBindingOption",
                             "NSValueTransformerBindingOption", "NSNullPlaceholderBindingOption", "NSNoSelectionPlaceholderBindingOption",
                             "NSMultipleValuesPlaceholderBindingOption", "NSNotApplicablePlaceholderBindingOption",
                             "NSValueBinding", "NSHiddenBinding", "NSEnabledBinding", "NSTitleBinding", "NSContentArrayBinding",
                             "NSContentBinding", "NSContentObjectBinding", "NSSelectionIndexesBinding", "NSMaxValueBinding",
                             "NSMinValueBinding", "NSEditableBinding", "NSToolTipBinding", "NSSortDescriptorsBinding"}) {
        static const std::map<std::string, std::string> values{
            {"NSObservedObjectKey", "NSObservedObject"}, {"NSObservedKeyPathKey", "NSObservedKeyPath"}, {"NSOptionsKey", "NSOptions"},
            {"NSValueTransformerNameBindingOption", "NSValueTransformerName"}, {"NSValueTransformerBindingOption", "NSValueTransformer"},
            {"NSNullPlaceholderBindingOption", "NSNullPlaceholder"}, {"NSNoSelectionPlaceholderBindingOption", "NSNoSelectionPlaceholder"},
            {"NSMultipleValuesPlaceholderBindingOption", "NSMultipleValuesPlaceholder"},
            {"NSNotApplicablePlaceholderBindingOption", "NSNotApplicablePlaceholder"},
            {"NSValueBinding", "value"}, {"NSHiddenBinding", "hidden"}, {"NSEnabledBinding", "enabled"}, {"NSTitleBinding", "title"},
            {"NSContentArrayBinding", "contentArray"}, {"NSContentBinding", "content"}, {"NSContentObjectBinding", "contentObject"},
            {"NSSelectionIndexesBinding", "selectionIndexes"}, {"NSMaxValueBinding", "maxValue"}, {"NSMinValueBinding", "minValue"},
            {"NSEditableBinding", "editable"}, {"NSToolTipBinding", "toolTip"}, {"NSSortDescriptorsBinding", "sortDescriptors"}};
        // (The NS*TransformerName constants are Foundation's: value_transformers.cpp.)
        const auto symbol = "_" + std::string(name);
        if (!support.process.resolve(appkit, symbol)) support.string_constant(appkit, symbol, values.at(name));
    }

    // ---- NSKeyValueBindingCreation ----
    support.method(root, "bind:toObject:withKeyPath:options:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3] || !s.x[4]) fail("NSInvalidArgumentException: -bind:toObject:withKeyPath:options: needs a binding, an object and a key path");
        self->bind(s.x[0], self->support.cf.utf8(s.x[2]), s.x[3], s.x[4], s.x[5]);
    }, "v@:@@@@");
    support.method(root, "unbind:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) self->unbind(s.x[0], self->support.cf.utf8(s.x[2]));
    }, "v@:@");
    support.method(root, "infoForBinding:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? self->info(s.x[0], self->support.cf.utf8(s.x[2])) : 0;
    }, "@@:@");
    support.method(root, "_alchemyPropagateValue:forBinding:", [self](CpuState& s, GuestMemory&) {
        if (s.x[3]) self->propagate(s.x[0], self->support.cf.utf8(s.x[3]), s.x[2]);
    }, "v@:@@");
    support.method(root, "exposeBinding:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        std::lock_guard lock(self->mutex);
        self->exposed[s.x[0]].push_back(self->support.cf.utf8(s.x[2]));
    }, "v@:@", true);
    const auto exposed_of = [self](Address cls) {
        std::vector<std::string> names;
        std::lock_guard lock(self->mutex);
        for (auto current = cls; current; current = self->support.send_id(current, "superclass"))
            if (const auto found = self->exposed.find(current); found != self->exposed.end())
                names.insert(names.end(), found->second.begin(), found->second.end());
        return names;
    };
    support.method(root, "exposedBindings", [self, exposed_of](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (const auto& name : exposed_of(self->support.objc.object_class(s.x[0]))) values.push_back(string(self->support, name));
        s.x[0] = array(self->support, values);
    }, "@@:");
    support.method(root, "exposedBindings", [self, exposed_of](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (const auto& name : exposed_of(s.x[0])) values.push_back(string(self->support, name));
        s.x[0] = array(self->support, values);
    }, "@@:", true);
    // nil: the class of a binding's value is not declared (AppKit's default for unknown bindings).
    support.method(root, "valueClassForBinding:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "#@:@");
    support.method(root, "optionDescriptionsForBinding:", [self](CpuState& s, GuestMemory&) { s.x[0] = array(self->support, {}); }, "@@:@");

    // ---- NSObjectController / NSArrayController ----
    if (!support.objc.find_class("NSController")) static_cast<void>(support.declare_class("NSController", "NSObject", appkit));
    const auto oc = self->object_controller = support.declare_class("NSObjectController", "NSController", appkit);
    const auto ac = self->array_controller = support.declare_class("NSArrayController", "NSObjectController", appkit);
    const auto proxy = self->proxy_class = support.declare_class("_NSControllerObjectProxy", "NSObject");
    const auto managed = support.declare_class("_NSManagedProxy", "NSObject", appkit);

    const auto setup = [self, ac](Address controller) {
        const auto state = self->state_of(controller);
        state->array = self->support.send(controller, "isKindOfClass:", [&] { CallRegs r; r.x[2] = ac; return r; }()).x0 & 0xff;
        if (state->array && !state->arranged) state->arranged = self->support.cf.array({});
        if (state->array && !state->selection_indexes) state->selection_indexes = self->index_set({});
        return state;
    };
    support.method(oc, "initWithContent:", [self, oc, setup](CpuState& s, GuestMemory&) {
        const auto controller = self->support.objc.send_super(s.x[0], oc, self->support.objc.selector("init")).x0;
        s.x[0] = controller;
        if (!controller) return;
        static_cast<void>(setup(controller));
        if (s.x[2]) self->content_changed(controller, objc_retain(self->support, s.x[2]));
    }, "@@:@");
    support.method(oc, "init", [self](CpuState& s, GuestMemory&) { s.x[0] = self->support.send_id(s.x[0], "initWithContent:", {0}); }, "@@:");
    support.method(oc, "initWithCoder:", [self, oc, setup](CpuState& s, GuestMemory&) {
        const auto controller = self->support.objc.send_super(s.x[0], oc, self->support.objc.selector("init")).x0;
        s.x[0] = controller;
        if (!controller) return;
        const auto coder = s.x[2];
        const auto state = setup(controller);
        const auto has = [&](std::string_view key) { return (self->support.send(coder, "containsValueForKey:", [&] { CallRegs r; r.x[2] = string(self->support, key); return r; }()).x0 & 0xff) != 0; };
        const auto flag = [&](std::string_view key, bool& out) { if (has(key)) out = (self->support.send(coder, "decodeBoolForKey:", [&] { CallRegs r; r.x[2] = string(self->support, key); return r; }()).x0 & 0xff) != 0; };
        flag("NSEditable", state->editable);
        flag("NSAutomaticallyPreparesContent", state->prepares_content);
        flag("NSAvoidsEmptySelection", state->avoids_empty_selection);
        flag("NSPreservesSelection", state->preserves_selection);
        flag("NSSelectsInsertedObjects", state->selects_inserted);
        flag("NSClearsFilterPredicateOnInsertion", state->clears_filter);
        flag("NSAutomaticallyRearrangesObjects", state->rearranges);
        // Not read: NSDeclaredKeys (Interface Builder's list of bound keys),
        // NSFilterRestrictsInsertion (inserted objects always pass the filter predicate).
        if (has("NSObjectClassName")) {
            const auto name = self->support.send_id(coder, "decodeObjectForKey:", {string(self->support, "NSObjectClassName")});
            if (name) state->object_class = self->support.call(foundation_library, "_NSClassFromString", {name});
        }
        // Interface Builder archives a _NSManagedProxy with every array
        // controller; only one naming an entity needs Core Data.
        if (has("_NSManagedProxy")) {
            const auto value = self->support.send_id(coder, "decodeObjectForKey:", {string(self->support, "_NSManagedProxy")});
            const auto entity = value && self->support.objc.responds(value, self->support.objc.selector("entityName"))
                ? self->support.send_id(value, "entityName") : 0;
            if (entity) unsupported("NSObjectController with the Core Data entity " + text(self->support, entity));
        }
    }, "@@:@");
    support.method(oc, "dealloc", [self, oc](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<ControllerState>(s.x[0])) {
            for (auto* slot : {&state->content, &state->selection_proxy, &state->arranged, &state->selection_indexes,
                               &state->sort_descriptors, &state->filter_predicate})
                objc_release(self->support, std::exchange(*slot, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], oc, self->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(oc, "content", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->content;
    }, "@@:");
    support.method(oc, "setContent:", [self, setup](CpuState& s, GuestMemory&) {
        static_cast<void>(setup(s.x[0]));
        self->content_changed(s.x[0], objc_retain(self->support, s.x[2]));
    }, "v@:@");
    support.method(oc, "selection", [self, proxy](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (!state->selection_proxy) {
            auto proxy_state = std::make_shared<ProxyState>();
            proxy_state->controller = s.x[0];
            state->selection_proxy = make_object(self->support, proxy, proxy_state);
        }
        s.x[0] = state->selection_proxy;
    }, "@@:");
    support.method(oc, "selectedObjects", [self](CpuState& s, GuestMemory&) { s.x[0] = array(self->support, self->selected(s.x[0])); }, "@@:");
    const auto flag_property = [self](Address cls, const char* getter, const char* setter, bool ControllerState::*member) {
        self->support.method(cls, getter, [self, member](CpuState& s, GuestMemory&) {
            const auto state = self->state_of(s.x[0]);
            s.x[0] = (*state).*member ? 1 : 0;
        }, "B@:");
        self->support.method(cls, setter, [self, member](CpuState& s, GuestMemory&) {
            const auto state = self->state_of(s.x[0]);
            (*state).*member = (s.x[2] & 0xff) != 0;
        }, "v@:B");
    };
    flag_property(oc, "isEditable", "setEditable:", &ControllerState::editable);
    flag_property(oc, "automaticallyPreparesContent", "setAutomaticallyPreparesContent:", &ControllerState::prepares_content);
    flag_property(ac, "avoidsEmptySelection", "setAvoidsEmptySelection:", &ControllerState::avoids_empty_selection);
    flag_property(ac, "preservesSelection", "setPreservesSelection:", &ControllerState::preserves_selection);
    flag_property(ac, "selectsInsertedObjects", "setSelectsInsertedObjects:", &ControllerState::selects_inserted);
    flag_property(ac, "clearsFilterPredicateOnInsertion", "setClearsFilterPredicateOnInsertion:", &ControllerState::clears_filter);
    flag_property(ac, "automaticallyRearrangesObjects", "setAutomaticallyRearrangesObjects:", &ControllerState::rearranges);
    support.method(oc, "objectClass", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        s.x[0] = state->object_class ? state->object_class : class_named(self->support, "NSMutableDictionary");
    }, "#@:");
    support.method(oc, "setObjectClass:", [self](CpuState& s, GuestMemory&) { self->state_of(s.x[0])->object_class = s.x[2]; }, "v@:#");
    support.method(oc, "newObject", [self](CpuState& s, GuestMemory&) {
        const auto cls = self->support.send_id(s.x[0], "objectClass");
        s.x[0] = self->support.send_id(self->support.send_id(cls, "alloc"), "init");
    }, "@@:");
    support.method(oc, "prepareContent", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        if (state->array) {
            const auto empty = self->support.cf.array({}, true);
            self->content_changed(s.x[0], empty);
        } else {
            self->content_changed(s.x[0], self->support.send_id(s.x[0], "newObject"));
        }
    }, "v@:");
    support.method(oc, "awakeFromNib", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        bool prepare;
        { std::lock_guard lock(state->mutex); prepare = state->prepares_content && !state->content; }
        if (prepare) static_cast<void>(self->support.send(s.x[0], "prepareContent"));
    }, "v@:");
    support.method(oc, "canAdd", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state_of(s.x[0])->editable; }, "B@:");
    support.method(oc, "canRemove", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->state_of(s.x[0])->editable && !self->selected(s.x[0]).empty();
    }, "B@:");
    support.method(oc, "addObject:", [self](CpuState& s, GuestMemory&) { self->content_changed(s.x[0], objc_retain(self->support, s.x[2])); }, "v@:@");
    support.method(oc, "removeObject:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        bool same;
        { std::lock_guard lock(state->mutex); same = state->content == s.x[2]; }
        if (same) self->content_changed(s.x[0], 0);
    }, "v@:@");
    support.method(oc, "add:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(s.x[0], "newObject");
        static_cast<void>(self->support.send_id(s.x[0], "addObject:", {object}));
        objc_release(self->support, object);
    }, "v@:@");
    support.method(oc, "remove:", [self](CpuState& s, GuestMemory&) {
        for (const auto object : self->selected(s.x[0])) static_cast<void>(self->support.send_id(s.x[0], "removeObject:", {object}));
    }, "v@:@");
    support.method(oc, "commitEditing", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");   // no editor holds uncommitted edits
    support.method(oc, "discardEditing", [](CpuState&, GuestMemory&) {}, "v@:");
    support.method(oc, "objectDidBeginEditing:", [](CpuState&, GuestMemory&) {}, "v@:@");
    support.method(oc, "objectDidEndEditing:", [](CpuState&, GuestMemory&) {}, "v@:@");
    support.method(oc, "isEditing", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    for (const auto* name : {"entityName", "managedObjectContext", "fetchPredicate", "usesLazyFetching"})
        support.method(oc, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // no Core Data entity
    for (const auto* name : {"setEntityName:", "setManagedObjectContext:", "setFetchPredicate:", "fetch:"})
        support.method(oc, name, [name = std::string(name)](CpuState&, GuestMemory&) {
            unsupported("-[NSObjectController " + name + "] (Core Data)");
        }, "v@:@");

    // NSArrayController
    support.method(ac, "arrangedObjects", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->arranged;
    }, "@@:");
    support.method(ac, "rearrangeObjects", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        Address content;
        { std::lock_guard lock(state->mutex); content = objc_retain(self->support, state->content); }
        self->content_changed(s.x[0], content);
    }, "v@:");
    support.method(ac, "arrangeObjects:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::vector<Address> values;
        for (const auto value : self->values_of(s.x[2])) {
            if (state->filter_predicate && !(self->support.send(state->filter_predicate, "evaluateWithObject:", [&] { CallRegs r; r.x[2] = value; return r; }()).x0 & 0xff))
                continue;
            values.push_back(value);
        }
        auto result = array(self->support, values);
        if (state->sort_descriptors && self->support.send_id(state->sort_descriptors, "count"))
            result = self->support.send_id(result, "sortedArrayUsingDescriptors:", {state->sort_descriptors});
        s.x[0] = result;
    }, "@@:@");
    support.method(ac, "selectionIndexes", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->selection_indexes;
    }, "@@:");
    support.method(ac, "setSelectionIndexes:", [self](CpuState& s, GuestMemory&) { self->set_selection(s.x[0], s.x[2]); s.x[0] = 1; }, "B@:@");
    support.method(ac, "selectionIndex", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        const auto indexes = self->indexes_of(state->selection_indexes);
        s.x[0] = indexes.empty() ? not_found : indexes.front();
    }, "Q@:");
    support.method(ac, "setSelectionIndex:", [self](CpuState& s, GuestMemory&) {
        const auto set = self->index_set(s.x[2] == not_found ? std::vector<Address>{} : std::vector<Address>{s.x[2]});
        self->set_selection(s.x[0], set);
        objc_release(self->support, set);
        s.x[0] = 1;
    }, "B@:Q");
    support.method(ac, "setSelectedObjects:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        std::vector<Address> indexes;
        {
            std::lock_guard lock(state->mutex);
            const auto arranged = self->values_of(state->arranged);
            for (const auto object : self->values_of(s.x[2]))
                for (std::size_t i = 0; i < arranged.size(); ++i) if (arranged[i] == object) { indexes.push_back(i); break; }
        }
        const auto set = self->index_set(indexes);
        self->set_selection(s.x[0], set);
        objc_release(self->support, set);
        s.x[0] = 1;
    }, "B@:@");
    support.method(ac, "addObject:", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[2];
        self->content_changed(s.x[0], self->edited_content(s.x[0], [object](std::vector<Address>& v) { v.push_back(object); }), true);
    }, "v@:@");
    support.method(ac, "addObjects:", [self](CpuState& s, GuestMemory&) {
        const auto added = self->values_of(s.x[2]);
        self->content_changed(s.x[0], self->edited_content(s.x[0], [&](std::vector<Address>& v) { v.insert(v.end(), added.begin(), added.end()); }), true);
    }, "v@:@");
    support.method(ac, "insertObject:atArrangedObjectIndex:", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[2];
        const auto index = s.x[3];
        self->content_changed(s.x[0], self->edited_content(s.x[0], [object, index](std::vector<Address>& v) {
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(index, v.size())), object);
        }), true);
    }, "v@:@Q");
    support.method(ac, "removeObject:", [self](CpuState& s, GuestMemory&) {
        const auto object = s.x[2];
        self->content_changed(s.x[0], self->edited_content(s.x[0], [object](std::vector<Address>& v) { std::erase(v, object); }));
    }, "v@:@");
    support.method(ac, "removeObjects:", [self](CpuState& s, GuestMemory&) {
        const auto removed = self->values_of(s.x[2]);
        self->content_changed(s.x[0], self->edited_content(s.x[0], [&](std::vector<Address>& v) {
            for (const auto object : removed) std::erase(v, object);
        }));
    }, "v@:@");
    support.method(ac, "removeObjectAtArrangedObjectIndex:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->state_of(s.x[0]);
        Address object{};
        {
            std::lock_guard lock(state->mutex);
            const auto arranged = self->values_of(state->arranged);
            if (s.x[2] >= arranged.size()) fail("NSRangeException: -removeObjectAtArrangedObjectIndex: index beyond bounds");
            object = arranged[s.x[2]];
        }
        static_cast<void>(self->support.send_id(s.x[0], "removeObject:", {object}));
    }, "v@:Q");
    support.method(ac, "remove:", [self](CpuState& s, GuestMemory&) {
        const auto list = self->selected(s.x[0]);
        std::vector<Address> values(list);
        const auto objects = array(self->support, values);
        static_cast<void>(self->support.send_id(s.x[0], "removeObjects:", {objects}));
    }, "v@:@");
    support.method(ac, "canInsert", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state_of(s.x[0])->editable; }, "B@:");
    support.method(ac, "insert:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(s.x[0], "newObject");
        static_cast<void>(self->support.send_id(s.x[0], "addObject:", {object}));
        objc_release(self->support, object);
    }, "v@:@");
    const auto step = [self](Address controller, int delta) {
        const auto state = self->state_of(controller);
        std::lock_guard lock(state->mutex);
        const auto indexes = self->indexes_of(state->selection_indexes);
        const auto count = self->values_of(state->arranged).size();
        if (indexes.empty()) return std::optional<Address>{};
        const auto next = static_cast<std::int64_t>(delta > 0 ? indexes.back() : indexes.front()) + delta;
        if (next < 0 || static_cast<std::size_t>(next) >= count) return std::optional<Address>{};
        return std::optional<Address>{static_cast<Address>(next)};
    };
    support.method(ac, "canSelectNext", [step](CpuState& s, GuestMemory&) { s.x[0] = step(s.x[0], 1).has_value(); }, "B@:");
    support.method(ac, "canSelectPrevious", [step](CpuState& s, GuestMemory&) { s.x[0] = step(s.x[0], -1).has_value(); }, "B@:");
    for (const auto& [name, delta] : std::array<std::pair<const char*, int>, 2>{{{"selectNext:", 1}, {"selectPrevious:", -1}}})
        support.method(ac, name, [self, step, delta = delta](CpuState& s, GuestMemory&) {
            if (const auto next = step(s.x[0], delta)) {
                const auto set = self->index_set({*next});
                self->set_selection(s.x[0], set);
                objc_release(self->support, set);
            }
        }, "v@:@");
    const auto object_property = [self](Address cls, const char* getter, const char* setter, Address ControllerState::*member, const char* key) {
        self->support.method(cls, getter, [self, member](CpuState& s, GuestMemory&) {
            const auto state = self->state_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = (*state).*member;
        }, "@@:");
        self->support.method(cls, setter, [self, member, key](CpuState& s, GuestMemory&) {
            const auto state = self->state_of(s.x[0]);
            const auto copy = s.x[2] ? self->support.send_id(s.x[2], "copy") : 0;
            self->will(s.x[0], {key});
            Address old;
            { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, copy); }
            objc_release(self->support, old);
            self->did(s.x[0], {key});
            // Arranged objects follow the new sort or filter.
            Address content;
            { std::lock_guard lock(state->mutex); content = objc_retain(self->support, state->content); }
            self->content_changed(s.x[0], content);
        }, "v@:@");
    };
    object_property(ac, "sortDescriptors", "setSortDescriptors:", &ControllerState::sort_descriptors, "sortDescriptors");
    object_property(ac, "filterPredicate", "setFilterPredicate:", &ControllerState::filter_predicate, "filterPredicate");

    // ---- the selection proxy ----
    support.method(proxy, "valueForKey:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.find_host_state<ProxyState>(s.x[0]);
        s.x[0] = state && s.x[2] ? self->selection_value(state->controller, s.x[2]) : 0;
    }, "@@:@");
    support.method(proxy, "setValue:forKey:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.find_host_state<ProxyState>(s.x[0]);
        if (!state) return;
        for (const auto object : self->selected(state->controller))
            static_cast<void>(self->support.send_id(object, "setValue:forKey:", {s.x[2], s.x[3]}));
    }, "v@:@@");
    // Observing "selection.<key>" observes <key> of each selected object.
    kvo::set_link_hook(support, proxy, [self](Address proxy_object, const std::string& key) {
        const auto state = self->support.objc.find_host_state<ProxyState>(proxy_object);
        if (!state) return;
        std::vector<std::uint64_t> old;
        {
            std::lock_guard lock(self->mutex);
            old = std::exchange(self->proxy_observations[{proxy_object, key}], {});
        }
        for (const auto id : old) kvo::unobserve(self->support, id);
        std::vector<std::uint64_t> ids;
        for (const auto object : self->selected(state->controller))
            ids.push_back(kvo::observe(self->support, object, key, kvo::option_prior, [self, proxy_object, key](Address, Address change) {
                const auto prior = self->support.send_id(change, "objectForKey:", {string(self->support, "notificationIsPrior")});
                static_cast<void>(self->support.send_id(proxy_object, prior ? "willChangeValueForKey:" : "didChangeValueForKey:", {string(self->support, key)}));
            }));
        std::lock_guard lock(self->mutex);
        self->proxy_observations[{proxy_object, key}] = std::move(ids);
    });

    // ---- _NSManagedProxy (nib placeholder for a controller's Core Data entity) ----
    support.method(managed, "initWithCoder:", [self, managed](CpuState& s, GuestMemory&) {
        const auto object = self->support.objc.send_super(s.x[0], managed, self->support.objc.selector("init")).x0;
        s.x[0] = object;
        if (!object) return;
        const auto name = self->support.send_id(s.x[2], "decodeObjectForKey:", {string(self->support, "NSEntityName")});
        self->support.objc.host_state<ManagedProxyState>(object)->entity_name = objc_retain(self->support, name);
    }, "@@:@");
    support.method(managed, "entityName", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.find_host_state<ManagedProxyState>(s.x[0]);
        s.x[0] = state ? state->entity_name : 0;
    }, "@@:");
}

} // namespace alchemy::gui::completion_kit
