#include "alchemy/darwin/foundation/kvo.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

// Cocoa bindings (NSKeyValueBindingCreation on NSObject) and the bindings
// controllers NSObjectController, NSArrayController, NSTreeController (with
// NSTreeNode; see the AppKit README) and _NSManagedProxy.
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
    // NSTreeController
    bool tree{}, rebuilding{}, always_multiple{};
    Address tree_root{};                                    // +1 NSTreeNode (represented object nil)
    Address children_key{}, count_key{}, leaf_key{};        // +1
    std::vector<std::vector<Address>> selection_paths;
    std::map<Address, std::uint64_t> observations;          // model object -> its children key path observation
};

struct ProxyState final : ObjectPayload { Address controller{}; };   // not retained (the controller owns the proxy)
struct ManagedProxyState final : ObjectPayload { Address entity_name{}; };
struct TreeNodeState final : ObjectPayload {
    Address represented{};                                  // +1
    Address parent{};                                       // not retained (the parent owns its children)
    std::vector<Address> children;                          // +1
    std::optional<bool> leaf;                               // the tree controller's leaf key path, when it has one
};

struct Bindings {
    explicit Bindings(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::pair<Address, std::string>, Binding> table;
    std::map<Address, std::vector<std::string>> exposed;   // class -> exposed bindings
    std::map<std::pair<Address, std::string>, std::vector<std::uint64_t>> proxy_observations;   // (proxy, key)
    Address no_selection{}, multiple_values{}, not_applicable{};
    Address object_controller{}, array_controller{}, proxy_class{}, tree_node_class{};

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
        if (state->tree) return tree_selected(*state);
        if (!state->array) return state->content ? std::vector<Address>{state->content} : std::vector<Address>{};
        const auto arranged = values_of(state->arranged);
        std::vector<Address> result;
        for (const auto index : indexes_of(state->selection_indexes)) if (index < arranged.size()) result.push_back(arranged[index]);
        return result;
    }
    // Content changed: rearranges and keeps (or resets) the selection.
    void content_changed(Address controller, Address new_content, bool select_inserted_last = false) {
        const auto state = state_of(controller);
        if (state->tree) { rebuild_tree(controller, std::nullopt, new_content); return; }
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
        if (state.tree) return tree_selected(state);
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
        if (objects.size() > 1 && state_of(controller)->always_multiple) return multiple_values;
        Address first{};
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto value = support.send_id(objects[index], "valueForKey:", {key});
            if (index == 0) first = value;
            else if (value != first && !(value && first && (support.send(value, "isEqual:", [&] { CallRegs r; r.x[2] = first; return r; }()).x0 & 0xff)))
                return multiple_values;
        }
        return first;
    }

    // ---- tree controllers ----
    std::shared_ptr<TreeNodeState> node_of(Address node) { return support.objc.host_state<TreeNodeState>(node); }
    // The node at `path` below `root` (0 when there is none).
    Address descendant(Address root, const std::vector<Address>& path) {
        auto current = root;
        for (const auto index : path) {
            if (!current) return 0;
            const auto children = node_of(current)->children;
            if (index >= children.size()) return 0;
            current = children[index];
        }
        return current;
    }
    std::vector<Address> path_values(Address path) {
        std::vector<Address> values;
        if (!path) return values;
        const auto length = support.send_id(path, "length");
        for (Address i = 0; i < length; ++i) values.push_back(support.send_id(path, "indexAtPosition:", {i}));
        return values;
    }
    Address path_object(const std::vector<Address>& values) {   // autoreleased
        const auto cls = class_named(support, "NSIndexPath");
        if (values.empty()) return support.send_id(cls, "indexPathWithIndexes:length:", {0, 0});
        auto path = support.send_id(cls, "indexPathWithIndex:", {values.front()});
        for (std::size_t i = 1; i < values.size(); ++i) path = support.send_id(path, "indexPathByAddingIndex:", {values[i]});
        return path;
    }
    // A node's index path below the root of its tree.
    std::vector<Address> path_of_node(Address node) {
        std::vector<Address> path;
        for (auto current = node; current;) {
            const auto parent = node_of(current)->parent;
            if (!parent) break;
            const auto siblings = node_of(parent)->children;
            const auto found = std::find(siblings.begin(), siblings.end(), current);
            if (found == siblings.end()) break;
            path.insert(path.begin(), static_cast<Address>(found - siblings.begin()));
            current = parent;
        }
        return path;
    }
    std::vector<Address> tree_selected(ControllerState& state) {
        std::vector<Address> result;
        for (const auto& path : state.selection_paths)
            if (const auto node = descendant(state.tree_root, path)) result.push_back(node_of(node)->represented);
        return result;
    }
    std::vector<Address> model_values(Address collection) {
        if (!collection || collection == null_object()) return {};
        if (support.objc.responds(collection, support.objc.selector("objectAtIndex:"))) return values_of(collection);
        if (support.objc.responds(collection, support.objc.selector("allObjects"))) return values_of(support.send_id(collection, "allObjects"));
        return {collection};
    }
    std::vector<Address> sorted_values(ControllerState& state, std::vector<Address> values) {
        if (!state.sort_descriptors || !support.send_id(state.sort_descriptors, "count") || values.size() < 2) return values;
        return values_of(support.send_id(array(support, values), "sortedArrayUsingDescriptors:", {state.sort_descriptors}));
    }
    // A +1 node for `object` with its subtree.
    Address build_node(ControllerState& state, Address object, Address parent, int depth) {
        const auto made = support.send_id(support.send_id(tree_node_class, "alloc"), "initWithRepresentedObject:", {object});
        const auto node = node_of(made);
        node->parent = parent;
        if (state.leaf_key) node->leaf = truth(support.send_id(object, "valueForKeyPath:", {state.leaf_key}));
        if (object && state.children_key && !node->leaf.value_or(false)) {
            if (depth > 256) fail("NSTreeController: the children key path nests more than 256 levels (a cycle?)");
            for (const auto child : sorted_values(state, model_values(support.send_id(object, "valueForKeyPath:", {state.children_key}))))
                node->children.push_back(build_node(state, child, made, depth + 1));
        }
        return made;
    }
    void paths_of(Address node, std::vector<Address>& prefix, const std::vector<Address>& objects, std::vector<std::vector<Address>>& out) {
        const auto children = node_of(node)->children;
        for (std::size_t i = 0; i < children.size(); ++i) {
            prefix.push_back(i);
            if (std::find(objects.begin(), objects.end(), node_of(children[i])->represented) != objects.end()) out.push_back(prefix);
            paths_of(children[i], prefix, objects, out);
            prefix.pop_back();
        }
    }
    void represented_below(Address node, std::set<Address>& out) {
        for (const auto child : node_of(node)->children) {
            if (const auto object = node_of(child)->represented) out.insert(object);
            represented_below(child, out);
        }
    }
    // Model objects' children change: the tree is rebuilt (unless the controller is the one changing them).
    void observe_tree(Address controller, ControllerState& state) {
        std::set<Address> objects;
        if (state.children_key && state.tree_root) represented_below(state.tree_root, objects);
        for (auto it = state.observations.begin(); it != state.observations.end();) {
            if (objects.contains(it->first)) { ++it; continue; }
            kvo::unobserve(support, it->second);
            it = state.observations.erase(it);
        }
        if (!state.children_key) return;
        const auto key = text(support, state.children_key);
        for (const auto object : objects)
            if (!state.observations.contains(object))
                state.observations[object] = kvo::observe(support, object, key, kvo::option_new, [this, controller](Address, Address) {
                    if (!state_of(controller)->rebuilding) rebuild_tree(controller, std::nullopt, std::nullopt);
                });
    }
    void unobserve_tree(ControllerState& state) {
        for (const auto& [object, id] : std::exchange(state.observations, {})) { static_cast<void>(object); kvo::unobserve(support, id); }
    }
    void tree_will(Address controller, bool everything) {
        if (everything) will(controller, {"content", "arrangedObjects", "selectionIndexPaths", "selectionIndexPath", "selectedNodes", "selectedObjects",
                                          "selection", "canRemove", "canAddChild", "canInsertChild"});
        else will(controller, {"selectionIndexPaths", "selectionIndexPath", "selectedNodes", "selectedObjects", "selection", "canRemove", "canAddChild",
                               "canInsertChild"});
    }
    void tree_did(Address controller, bool everything) {
        if (everything) did(controller, {"content", "arrangedObjects", "selectionIndexPaths", "selectionIndexPath", "selectedNodes", "selectedObjects",
                                         "selection", "canRemove", "canAddChild", "canInsertChild"});
        else did(controller, {"selectionIndexPaths", "selectionIndexPath", "selectedNodes", "selectedObjects", "selection", "canRemove", "canAddChild",
                              "canInsertChild"});
    }
    // Rebuilds the arranged tree (after `new_content`, +1, replaces the content when given), keeping
    // the selected objects selected, or selecting `select` when given.
    void rebuild_tree(Address controller, std::optional<std::vector<Address>> select, std::optional<Address> new_content, bool notify = true) {
        const auto state = state_of(controller);
        if (notify) tree_will(controller, true);
        {
            std::lock_guard lock(state->mutex);
            const auto previous = select ? *select : tree_selected(*state);
            if (new_content) support.release(std::exchange(state->content, *new_content));
            state->rebuilding = true;
            try {
                const auto root = support.send_id(support.send_id(tree_node_class, "alloc"), "initWithRepresentedObject:", {0});
                for (const auto object : sorted_values(*state, content_values(*state)))
                    node_of(root)->children.push_back(build_node(*state, object, root, 0));
                objc_release(support, std::exchange(state->tree_root, root));
                std::vector<std::vector<Address>> paths;
                std::vector<Address> prefix;
                if (!previous.empty()) paths_of(root, prefix, previous, paths);
                if (paths.empty() && state->avoids_empty_selection && !node_of(root)->children.empty()) paths.push_back({0});
                state->selection_paths = std::move(paths);
                observe_tree(controller, *state);
            } catch (...) {
                state->rebuilding = false;
                throw;
            }
            state->rebuilding = false;
        }
        if (notify) tree_did(controller, true);
    }
    Address tree_root(Address controller) {
        const auto state = state_of(controller);
        std::lock_guard lock(state->mutex);
        if (!state->tree_root) rebuild_tree(controller, std::nullopt, std::nullopt, false);
        return state->tree_root;
    }
    void set_tree_selection(Address controller, std::vector<std::vector<Address>> paths) {
        const auto state = state_of(controller);
        tree_will(controller, false);
        {
            std::lock_guard lock(state->mutex);
            std::erase_if(paths, [&](const std::vector<Address>& path) { return path.empty() || !descendant(state->tree_root, path); });
            std::sort(paths.begin(), paths.end());
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
            if (paths.empty() && state->avoids_empty_selection && state->tree_root && !node_of(state->tree_root)->children.empty()) paths.push_back({0});
            state->selection_paths = std::move(paths);
        }
        tree_did(controller, false);
    }
    // The mutable children of a node's represented object (its children key path).
    Address model_children(ControllerState& state, Address object) {
        if (!state.children_key) fail("NSInternalInconsistencyException: NSTreeController has no children key path");
        return support.send_id(object, "mutableArrayValueForKeyPath:", {state.children_key});
    }
    // Inserts `objects` at `path` in the model (the last index counts in the model's order), then rebuilds.
    void tree_insert(Address controller, const std::vector<Address>& path, const std::vector<Address>& objects) {
        if (path.empty()) fail("NSInternalInconsistencyException: NSTreeController: an empty insertion index path");
        const auto state = state_of(controller);
        const auto root = tree_root(controller);
        const auto parent = descendant(root, {path.begin(), path.end() - 1});
        if (!parent) fail("NSRangeException: NSTreeController: no node at the insertion index path's parent");
        auto index = path.back();
        if (parent == root) {
            std::vector<Address> values;
            { std::lock_guard lock(state->mutex); values = content_values(*state); }
            index = std::min<Address>(index, values.size());
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), objects.begin(), objects.end());
            rebuild_tree(controller, state->selects_inserted ? std::optional(objects) : std::nullopt, support.cf.array(values, true));
            return;
        }
        state->rebuilding = true;
        try {
            const auto children = model_children(*state, node_of(parent)->represented);
            index = std::min<Address>(index, support.send_id(children, "count"));
            for (const auto object : objects) static_cast<void>(support.send_id(children, "insertObject:atIndex:", {object, index++}));
        } catch (...) {
            state->rebuilding = false;
            throw;
        }
        state->rebuilding = false;
        rebuild_tree(controller, state->selects_inserted ? std::optional(objects) : std::nullopt, std::nullopt);
    }
    // Removes the nodes' represented objects from the model, then rebuilds (keeping what stays selected).
    void tree_remove(Address controller, const std::vector<Address>& nodes, bool rebuild = true) {
        const auto state = state_of(controller);
        const auto root = tree_root(controller);
        std::vector<Address> roots;
        state->rebuilding = true;
        try {
            for (const auto node : nodes) {
                const auto parent = node_of(node)->parent;
                const auto object = node_of(node)->represented;
                if (!parent || parent == root) { roots.push_back(object); continue; }
                const auto children = model_children(*state, node_of(parent)->represented);
                const auto values = values_of(children);
                if (const auto found = std::find(values.begin(), values.end(), object); found != values.end())
                    static_cast<void>(support.send_id(children, "removeObjectAtIndex:", {static_cast<Address>(found - values.begin())}));
            }
        } catch (...) {
            state->rebuilding = false;
            throw;
        }
        state->rebuilding = false;
        std::optional<Address> content;
        if (!roots.empty()) {
            std::vector<Address> values;
            { std::lock_guard lock(state->mutex); values = content_values(*state); }
            for (const auto object : roots)
                if (const auto found = std::find(values.begin(), values.end(), object); found != values.end()) values.erase(found);
            content = support.cf.array(values, true);
        }
        if (rebuild || content) rebuild_tree(controller, std::nullopt, content);
    }
    std::vector<Address> nodes_at(Address controller, Address paths_array) {
        const auto root = tree_root(controller);
        std::vector<Address> nodes;
        for (const auto path : values_of(paths_array)) {
            const auto node = descendant(root, path_values(path));
            if (!node || node == root) fail("NSRangeException: NSTreeController: no node at an index path");
            nodes.push_back(node);
        }
        return nodes;
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
    const auto tc = support.declare_class("NSTreeController", "NSObjectController", appkit);
    const auto proxy = self->proxy_class = support.declare_class("_NSControllerObjectProxy", "NSObject");
    const auto managed = support.declare_class("_NSManagedProxy", "NSObject", appkit);

    const auto setup = [self, ac, tc](Address controller) {
        const auto state = self->state_of(controller);
        state->array = self->support.send(controller, "isKindOfClass:", [&] { CallRegs r; r.x[2] = ac; return r; }()).x0 & 0xff;
        state->tree = self->support.send(controller, "isKindOfClass:", [&] { CallRegs r; r.x[2] = tc; return r; }()).x0 & 0xff;
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

    // ---- NSTreeNode ----
    const auto tn = self->tree_node_class = support.declare_class("NSTreeNode", "NSObject", appkit);
    const auto node_of = [self](Address node) { return self->node_of(node); };
    support.method(tn, "initWithRepresentedObject:", [self, tn](CpuState& s, GuestMemory&) {
        const auto node = self->support.objc.send_super(s.x[0], tn, self->support.objc.selector("init")).x0;
        s.x[0] = node;
        if (node) self->node_of(node)->represented = objc_retain(self->support, s.x[2]);
    }, "@@:@");
    support.method(tn, "init", [self](CpuState& s, GuestMemory&) { s.x[0] = self->support.send_id(s.x[0], "initWithRepresentedObject:", {0}); }, "@@:");
    support.method(tn, "treeNodeWithRepresentedObject:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "initWithRepresentedObject:", {s.x[2]}));
    }, "@@:@", true);
    support.method(tn, "dealloc", [self, tn](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<TreeNodeState>(s.x[0])) {
            objc_release(self->support, std::exchange(state->represented, 0));
            for (const auto child : std::exchange(state->children, {})) {
                if (const auto child_state = self->support.objc.find_host_state<TreeNodeState>(child); child_state && child_state->parent == s.x[0])
                    child_state->parent = 0;
                objc_release(self->support, child);
            }
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], tn, self->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(tn, "representedObject", [node_of](CpuState& s, GuestMemory&) { s.x[0] = node_of(s.x[0])->represented; }, "@@:");
    support.method(tn, "parentNode", [node_of](CpuState& s, GuestMemory&) { s.x[0] = node_of(s.x[0])->parent; }, "@@:");
    support.method(tn, "childNodes", [self, node_of](CpuState& s, GuestMemory&) { s.x[0] = array(self->support, node_of(s.x[0])->children); }, "@@:");
    // Edits through the proxy go through the childNodes accessors below.
    support.method(tn, "mutableChildNodes", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.send_id(s.x[0], "mutableArrayValueForKey:", {string(self->support, "childNodes")});
    }, "@@:");
    support.method(tn, "countOfChildNodes", [node_of](CpuState& s, GuestMemory&) { s.x[0] = node_of(s.x[0])->children.size(); }, "Q@:");
    support.method(tn, "objectInChildNodesAtIndex:", [node_of](CpuState& s, GuestMemory&) {
        const auto state = node_of(s.x[0]);
        if (s.x[2] >= state->children.size()) fail("NSRangeException: -[NSTreeNode objectInChildNodesAtIndex:] index beyond bounds");
        s.x[0] = state->children[s.x[2]];
    }, "@@:Q");
    // A node keeps one parent: inserting it elsewhere takes it from its old parent.
    const auto detach = [self, node_of](Address child) {
        const auto parent = node_of(child)->parent;
        if (!parent) return;
        auto& siblings = node_of(parent)->children;
        if (const auto found = std::find(siblings.begin(), siblings.end(), child); found != siblings.end()) {
            siblings.erase(found);
            objc_release(self->support, child);
        }
        node_of(child)->parent = 0;
    };
    support.method(tn, "insertObject:inChildNodesAtIndex:", [self, node_of, detach](CpuState& s, GuestMemory&) {
        const auto child = s.x[2];
        if (!child) fail("NSInvalidArgumentException: -[NSTreeNode insertObject:inChildNodesAtIndex:] nil node");
        if (s.x[3] > node_of(s.x[0])->children.size()) fail("NSRangeException: -[NSTreeNode insertObject:inChildNodesAtIndex:] index beyond bounds");
        objc_retain(self->support, child);
        detach(child);
        auto& children = node_of(s.x[0])->children;
        children.insert(children.begin() + static_cast<std::ptrdiff_t>(std::min<Address>(s.x[3], children.size())), child);
        node_of(child)->parent = s.x[0];
    }, "v@:@Q");
    support.method(tn, "removeObjectFromChildNodesAtIndex:", [node_of, detach](CpuState& s, GuestMemory&) {
        const auto children = node_of(s.x[0])->children;
        if (s.x[2] >= children.size()) fail("NSRangeException: -[NSTreeNode removeObjectFromChildNodesAtIndex:] index beyond bounds");
        detach(children[s.x[2]]);
    }, "v@:Q");
    support.method(tn, "replaceObjectInChildNodesAtIndex:withObject:", [self](CpuState& s, GuestMemory&) {
        const auto self_node = s.x[0], index = s.x[2], child = s.x[3];
        static_cast<void>(self->support.send_id(self_node, "removeObjectFromChildNodesAtIndex:", {index}));
        static_cast<void>(self->support.send_id(self_node, "insertObject:inChildNodesAtIndex:", {child, index}));
    }, "v@:Q@");
    support.method(tn, "isLeaf", [node_of](CpuState& s, GuestMemory&) {
        const auto state = node_of(s.x[0]);
        s.x[0] = state->leaf ? *state->leaf : state->children.empty();
    }, "B@:");
    support.method(tn, "indexPath", [self](CpuState& s, GuestMemory&) { s.x[0] = self->path_object(self->path_of_node(s.x[0])); }, "@@:");
    support.method(tn, "descendantNodeAtIndexPath:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? self->descendant(s.x[0], self->path_values(s.x[2])) : 0;
    }, "@@:@");
    // Sorts the child nodes themselves (the descriptors' keys are the nodes' keys: "representedObject.name").
    support.method(tn, "sortWithSortDescriptors:recursive:", [self, node_of](CpuState& s, GuestMemory&) {
        const auto descriptors = s.x[2];
        const bool recursive = (s.x[3] & 0xff) != 0;
        std::vector<Address> pending{s.x[0]};
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            auto& children = node_of(node)->children;
            if (descriptors && children.size() > 1)
                children = self->values_of(self->support.send_id(array(self->support, children), "sortedArrayUsingDescriptors:", {descriptors}));
            if (recursive) pending.insert(pending.end(), children.begin(), children.end());
        }
    }, "v@:@B");

    // ---- NSTreeController ----
    // (decoded from a nib: refused, its archive keys are not documented)
    support.method(tc, "initWithCoder:", [](CpuState&, GuestMemory&) {
        unsupported("NSTreeController decoded from a nib (its archived children / count / leaf key paths are not documented)");
    }, "@@:@");
    support.method(tc, "dealloc", [self, tc](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<ControllerState>(s.x[0])) {
            self->unobserve_tree(*state);
            for (auto* slot : {&state->tree_root, &state->children_key, &state->count_key, &state->leaf_key}) objc_release(self->support, std::exchange(*slot, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], tc, self->support.objc.selector("dealloc")));
    }, "v@:");
    for (const auto& [getter, setter, member] : std::array<std::tuple<const char*, const char*, Address ControllerState::*>, 3>{{
             {"childrenKeyPath", "setChildrenKeyPath:", &ControllerState::children_key},
             {"countKeyPath", "setCountKeyPath:", &ControllerState::count_key},
             {"leafKeyPath", "setLeafKeyPath:", &ControllerState::leaf_key}}}) {
        support.method(tc, getter, [self, member = member](CpuState& s, GuestMemory&) { s.x[0] = (*self->state_of(s.x[0])).*member; }, "@@:");
        support.method(tc, std::string(getter) + "ForNode:", [self, member = member](CpuState& s, GuestMemory&) { s.x[0] = (*self->state_of(s.x[0])).*member; }, "@@:@");
        support.method(tc, setter, [self, setup, member = member](CpuState& s, GuestMemory&) {
            const auto state = setup(s.x[0]);
            const auto copy = s.x[2] ? self->support.send_id(s.x[2], "copy") : 0;
            {
                std::lock_guard lock(state->mutex);
                self->unobserve_tree(*state);
                objc_release(self->support, std::exchange((*state).*member, copy));
            }
            self->rebuild_tree(s.x[0], std::nullopt, std::nullopt);
        }, "v@:@");
    }
    support.method(tc, "arrangedObjects", [self, setup](CpuState& s, GuestMemory&) {
        static_cast<void>(setup(s.x[0]));
        s.x[0] = self->tree_root(s.x[0]);
    }, "@@:");
    support.method(tc, "rearrangeObjects", [self, setup](CpuState& s, GuestMemory&) {
        static_cast<void>(setup(s.x[0]));
        self->rebuild_tree(s.x[0], std::nullopt, std::nullopt);
    }, "v@:");
    flag_property(tc, "avoidsEmptySelection", "setAvoidsEmptySelection:", &ControllerState::avoids_empty_selection);
    flag_property(tc, "preservesSelection", "setPreservesSelection:", &ControllerState::preserves_selection);
    flag_property(tc, "selectsInsertedObjects", "setSelectsInsertedObjects:", &ControllerState::selects_inserted);
    flag_property(tc, "alwaysUsesMultipleValuesMarker", "setAlwaysUsesMultipleValuesMarker:", &ControllerState::always_multiple);
    object_property(tc, "sortDescriptors", "setSortDescriptors:", &ControllerState::sort_descriptors, "sortDescriptors");
    // Selection.
    const auto paths_array = [self](Address controller) {
        const auto state = self->state_of(controller);
        std::vector<std::vector<Address>> paths;
        { std::lock_guard lock(state->mutex); paths = state->selection_paths; }
        std::vector<Address> objects;
        for (const auto& path : paths) objects.push_back(self->path_object(path));
        return objects;
    };
    const auto paths_from = [self](Address array_object) {
        std::vector<std::vector<Address>> paths;
        for (const auto path : self->values_of(array_object)) paths.push_back(self->path_values(path));
        return paths;
    };
    support.method(tc, "selectionIndexPaths", [self, paths_array](CpuState& s, GuestMemory&) { s.x[0] = array(self->support, paths_array(s.x[0])); }, "@@:");
    support.method(tc, "selectionIndexPath", [paths_array](CpuState& s, GuestMemory&) {
        const auto paths = paths_array(s.x[0]);
        s.x[0] = paths.empty() ? 0 : paths.front();
    }, "@@:");
    support.method(tc, "setSelectionIndexPaths:", [self, paths_from](CpuState& s, GuestMemory&) {
        static_cast<void>(self->tree_root(s.x[0]));
        self->set_tree_selection(s.x[0], paths_from(s.x[2]));
        s.x[0] = 1;
    }, "B@:@");
    support.method(tc, "setSelectionIndexPath:", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->tree_root(s.x[0]));
        self->set_tree_selection(s.x[0], s.x[2] ? std::vector<std::vector<Address>>{self->path_values(s.x[2])} : std::vector<std::vector<Address>>{});
        s.x[0] = 1;
    }, "B@:@");
    for (const bool add : {true, false})
        support.method(tc, add ? "addSelectionIndexPaths:" : "removeSelectionIndexPaths:", [self, paths_from, add](CpuState& s, GuestMemory&) {
            static_cast<void>(self->tree_root(s.x[0]));
            const auto state = self->state_of(s.x[0]);
            std::vector<std::vector<Address>> paths;
            { std::lock_guard lock(state->mutex); paths = state->selection_paths; }
            for (const auto& path : paths_from(s.x[2])) {
                if (add) paths.push_back(path);
                else std::erase(paths, path);
            }
            self->set_tree_selection(s.x[0], std::move(paths));
            s.x[0] = 1;
        }, "B@:@");
    support.method(tc, "selectedNodes", [self](CpuState& s, GuestMemory&) {
        const auto root = self->tree_root(s.x[0]);
        const auto state = self->state_of(s.x[0]);
        std::vector<Address> nodes;
        std::lock_guard lock(state->mutex);
        for (const auto& path : state->selection_paths)
            if (const auto node = self->descendant(root, path)) nodes.push_back(node);
        s.x[0] = array(self->support, nodes);
    }, "@@:");
    // Editing. Index paths name arranged positions; the last index of an insertion counts in the model's order.
    const auto selected_paths = [self](Address controller) {
        static_cast<void>(self->tree_root(controller));
        const auto state = self->state_of(controller);
        std::lock_guard lock(state->mutex);
        return state->selection_paths;
    };
    const auto child_count = [self](Address controller, const std::vector<Address>& path) -> Address {
        const auto node = self->descendant(self->tree_root(controller), path);
        if (!node) return 0;
        const auto state = self->state_of(controller);
        const auto object = self->node_of(node)->represented;
        if (!object) return self->model_values(state->content).size();
        return state->children_key ? self->model_values(self->support.send_id(object, "valueForKeyPath:", {state->children_key})).size() : 0;
    };
    // add: / addObject:: at the end of the selection's level (the top level without a selection).
    const auto add_object = [self, selected_paths, child_count](Address controller, Address object) {
        auto paths = selected_paths(controller);
        std::vector<Address> parent = paths.empty() ? std::vector<Address>{} : std::vector<Address>(paths.front().begin(), paths.front().end() - 1);
        auto at = parent;
        at.push_back(child_count(controller, parent));
        self->tree_insert(controller, at, {object});
    };
    support.method(tc, "addObject:", [add_object](CpuState& s, GuestMemory&) { add_object(s.x[0], s.x[2]); }, "v@:@");
    const auto with_new = [self](Address controller, const auto& action) {
        const auto object = self->support.send_id(controller, "newObject");
        try { action(object); } catch (...) { objc_release(self->support, object); throw; }
        objc_release(self->support, object);
    };
    support.method(tc, "add:", [with_new, add_object](CpuState& s, GuestMemory&) {
        const auto controller = s.x[0];
        with_new(controller, [&](Address object) { add_object(controller, object); });
    }, "v@:@");
    // insert:: at the selection (the end of the top level without one).
    support.method(tc, "insert:", [self, with_new, selected_paths, child_count](CpuState& s, GuestMemory&) {
        const auto controller = s.x[0];
        const auto paths = selected_paths(controller);
        const auto at = paths.empty() ? std::vector<Address>{child_count(controller, {})} : paths.front();
        with_new(controller, [&](Address object) { self->tree_insert(controller, at, {object}); });
    }, "v@:@");
    // addChild: / insertChild:: the selected node's last child (refused without exactly one selected container).
    for (const auto* name : {"addChild:", "insertChild:"})
        support.method(tc, name, [self, with_new, selected_paths, child_count](CpuState& s, GuestMemory&) {
            const auto controller = s.x[0];
            const auto paths = selected_paths(controller);
            if (paths.size() != 1) fail("NSInternalInconsistencyException: NSTreeController: adding a child needs exactly one selected node");
            auto at = paths.front();
            at.push_back(child_count(controller, paths.front()));
            with_new(controller, [&](Address object) { self->tree_insert(controller, at, {object}); });
        }, "v@:@");
    const auto can_child = [self, selected_paths](Address controller) {
        const auto state = self->state_of(controller);
        const auto paths = selected_paths(controller);
        if (!state->editable || paths.size() != 1 || !state->children_key) return false;
        const auto node = self->descendant(state->tree_root, paths.front());
        return node && !self->node_of(node)->leaf.value_or(false);
    };
    for (const auto* name : {"canAddChild", "canInsertChild"})
        support.method(tc, name, [can_child](CpuState& s, GuestMemory&) { s.x[0] = can_child(s.x[0]); }, "B@:");
    support.method(tc, "canInsert", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state_of(s.x[0])->editable; }, "B@:");
    support.method(tc, "insertObject:atArrangedObjectIndexPath:", [self](CpuState& s, GuestMemory&) {
        self->tree_insert(s.x[0], self->path_values(s.x[3]), {s.x[2]});
    }, "v@:@@");
    // Each object at its index path, in order.
    support.method(tc, "insertObjects:atArrangedObjectIndexPaths:", [self](CpuState& s, GuestMemory&) {
        const auto objects = self->values_of(s.x[2]);
        const auto paths = self->values_of(s.x[3]);
        if (objects.size() != paths.size()) fail("NSInvalidArgumentException: -insertObjects:atArrangedObjectIndexPaths: needs one index path per object");
        for (std::size_t i = 0; i < objects.size(); ++i) self->tree_insert(s.x[0], self->path_values(paths[i]), {objects[i]});
    }, "v@:@@");
    support.method(tc, "removeObjectAtArrangedObjectIndexPath:", [self](CpuState& s, GuestMemory&) {
        const auto list = array(self->support, std::vector<Address>{s.x[2]});
        self->tree_remove(s.x[0], self->nodes_at(s.x[0], list));
    }, "v@:@");
    support.method(tc, "removeObjectsAtArrangedObjectIndexPaths:", [self](CpuState& s, GuestMemory&) {
        self->tree_remove(s.x[0], self->nodes_at(s.x[0], s.x[2]));
    }, "v@:@");
    support.method(tc, "remove:", [self](CpuState& s, GuestMemory&) {
        const auto root = self->tree_root(s.x[0]);
        const auto state = self->state_of(s.x[0]);
        std::vector<Address> nodes;
        {
            std::lock_guard lock(state->mutex);
            for (const auto& path : state->selection_paths)
                if (const auto node = self->descendant(root, path)) nodes.push_back(node);
        }
        if (!nodes.empty()) self->tree_remove(s.x[0], nodes);
    }, "v@:@");
    support.method(tc, "removeObject:", [self](CpuState& s, GuestMemory&) {
        const auto root = self->tree_root(s.x[0]);
        std::vector<std::vector<Address>> paths;
        std::vector<Address> prefix;
        self->paths_of(root, prefix, {s.x[2]}, paths);
        std::vector<Address> nodes;
        for (const auto& path : paths) nodes.push_back(self->descendant(root, path));
        if (!nodes.empty()) self->tree_remove(s.x[0], nodes);
    }, "v@:@");
    // Moves: the nodes leave their places, then go in at the destination (an index path in the tree without them).
    const auto move_nodes = [self](Address controller, const std::vector<Address>& nodes, Address destination) {
        std::vector<Address> objects;
        for (const auto node : nodes) objects.push_back(objc_retain(self->support, self->node_of(node)->represented));
        try {
            self->tree_remove(controller, nodes);
            self->tree_insert(controller, self->path_values(destination), objects);
        } catch (...) {
            for (const auto object : objects) objc_release(self->support, object);
            throw;
        }
        for (const auto object : objects) objc_release(self->support, object);
    };
    support.method(tc, "moveNode:toIndexPath:", [move_nodes](CpuState& s, GuestMemory&) { move_nodes(s.x[0], {s.x[2]}, s.x[3]); }, "v@:@@");
    support.method(tc, "moveNodes:toIndexPath:", [self, move_nodes](CpuState& s, GuestMemory&) { move_nodes(s.x[0], self->values_of(s.x[2]), s.x[3]); }, "v@:@@");

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
