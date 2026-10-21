#include "alchemy/darwin/foundation/kvo.hpp"

#include <map>
#include <set>

// Key-value observing on NSObject (Foundation's NSKeyValueObserving),
// the only implementation (Qt's QMacKeyValueObserver, Cocoa bindings and
// NSOperation dependencies all observe through it).
//
// Registration: -addObserver:forKeyPath:options:context: observes every key
// of the path: the root for the first key, the first key's value for the
// second, and so on; a change of an intermediate key re-attaches the rest of
// the path to the new value. Options New, Old, Initial and Prior are honoured;
// nil values appear as NSNull in the change dictionary, as documented.
// -removeObserver:forKeyPath:(context:) removes the newest matching
// registration and fails explicitly (Foundation raises) when there is none.
//
// Notification: -willChangeValueForKey: / -didChangeValueForKey: and the
// indexed (-willChange:valuesAtIndexes:forKey:) and set-mutation variants
// notify the observers of that key of that object (nested and interleaved
// pairs pair up per thread, newest first). Notifications run on the thread
// that made the change, as on macOS.
//
// Automatic notification: when an object of a class whose
// +automaticallyNotifiesObserversForKey: answers YES (NSObject's default
// consults +automaticallyNotifiesObserversOf<Key>) is observed, its isa
// becomes NSKVONotifying_<Class>, a subclass whose -set<Key>: and indexed
// mutators (-insertObject:in<Key>AtIndex:, -removeObjectFrom<Key>AtIndex:,
// -replaceObjectIn<Key>AtIndex:withObject:, -insert<Key>:atIndexes:,
// -remove<Key>AtIndexes:, -replace<Key>AtIndexes:with<Key>:) wrap the
// original in the will/did pair; its -class answers the original class.
// Swift classes, CoreFoundation-backed objects and objects outside the object
// store are not subclassed (their changes notify only through will/did).
// Dependent keys come from +keyPathsForValuesAffectingValueForKey: (default:
// +keyPathsForValuesAffecting<Key>).
//
// Deallocation: registrations rooted at an object end when it is deallocated,
// and links through it are dropped (macOS 10.13 and later behave so).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct Registration {
    std::uint64_t id{};
    Address root{}, observer{}, context{}, path_string{};   // path_string: +1 copy
    std::string path;
    std::vector<std::string> keys;
    std::uint64_t options{};
    kvo::Callback callback;                       // internal observation
    std::function<void(bool prior)> dependent;    // dependent-key observation
    std::vector<Address> objects;                 // objects[d] is observed for keys[d]
    std::vector<std::vector<std::uint64_t>> dependents;   // per depth
    bool alive{true};
};

struct Link { std::uint64_t id{}; std::size_t depth{}; };

struct Pending {
    Address object{};
    std::string key;
    std::vector<std::pair<std::uint64_t, Address>> old;   // registration id -> +1 old value (0: none)
    std::vector<std::pair<std::shared_ptr<Registration>, std::size_t>> registrations;
};

thread_local std::vector<Pending> pending_changes;

std::string capitalized(std::string key) {
    if (!key.empty() && key[0] >= 'a' && key[0] <= 'z') key[0] = static_cast<char>(key[0] - 'a' + 'A');
    return key;
}

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> keys;
    std::size_t start = 0;
    for (;;) {
        const auto dot = path.find('.', start);
        keys.push_back(path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return keys;
}

struct Kvo {
    explicit Kvo(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Registration>> registrations;
    std::map<std::pair<Address, std::string>, std::vector<Link>> links;
    std::uint64_t next{1};
    std::map<Address, Address> kvo_classes;   // original -> NSKVONotifying_ subclass
    std::map<Address, Address> originals;     // subclass -> original
    std::set<std::pair<Address, std::string>> overridden;   // (subclass, selector)
    std::map<std::pair<Address, std::string>, bool> automatic;   // (original class, key)
    std::map<std::string, Address> keys;      // immortal key strings
    std::set<std::pair<Address, std::string>> hooked;   // dealloc hooks (object, tag)
    std::map<Address, Address> observation_info;
    std::map<std::pair<Address, std::string>, std::vector<std::string>> affecting_cache;   // (original class, key)
    std::map<Address, kvo::LinkHook> link_hooks;   // class -> hook for links to its instances
    int dependent_depth{};

    Address key_string(const std::string& key) {
        std::lock_guard lock(mutex);
        auto& slot = keys[key];
        if (!slot) slot = support.cf.constant_string(key);
        return slot;
    }
    Address null_object() { return support.send_id(class_named(support, "NSNull"), "null"); }
    Address value_for_key(Address object, const std::string& key) {
        return object ? support.send_id(object, "valueForKey:", {key_string(key)}) : 0;
    }
    Address path_value(const Registration& reg) {
        return reg.root ? support.send_id(reg.root, "valueForKeyPath:", {reg.path_string}) : 0;
    }
    Address original_class(Address cls) {
        std::lock_guard lock(mutex);
        const auto found = originals.find(cls);
        return found == originals.end() ? cls : found->second;
    }

    // ---- dealloc hooks ----
    void on_dealloc(Address object, std::string_view tag, std::function<void(Address)> cleanup) {
        if (!object) return;
        const auto record = support.process.objects().find(object);
        if (!record || record->immortal) return;
        {
            std::lock_guard lock(mutex);
            if (!hooked.insert({object, std::string(tag)}).second) return;
        }
        auto previous = record->destroy;
        record->destroy = [this, previous, cleanup = std::move(cleanup), tag = std::string(tag)](Address address) {
            struct After {
                Kvo& self; Address address; const std::string& tag; const std::function<void(Address)>& cleanup;
                ~After() {
                    { std::lock_guard lock(self.mutex); self.hooked.erase({address, tag}); }
                    try { cleanup(address); } catch (...) {}
                }
            } after{*this, address, tag, cleanup};
            if (previous) previous(address);
        };
    }
    void object_deallocated(Address object) {
        std::vector<std::shared_ptr<Registration>> rooted;
        {
            std::lock_guard lock(mutex);
            observation_info.erase(object);
            for (auto it = links.begin(); it != links.end();) {
                if (it->first.first == object) it = links.erase(it); else ++it;
            }
            for (auto& [id, reg] : registrations) {
                static_cast<void>(id);
                if (reg->root == object) rooted.push_back(reg);
                for (auto& value : reg->objects) if (value == object) value = 0;
            }
        }
        for (const auto& reg : rooted) remove(reg);
    }

    // ---- automatic notification (NSKVONotifying_ subclasses) ----
    bool swift_class(Address cls) {
        try { return (support.memory().read64(cls + 32) & 3) != 0; } catch (const std::exception&) { return true; }
    }
    bool automatic_for(Address cls, const std::string& key) {
        {
            std::lock_guard lock(mutex);
            if (const auto found = automatic.find({cls, key}); found != automatic.end()) return found->second;
        }
        const bool value = (support.send(cls, "automaticallyNotifiesObserversForKey:", [&] {
            CallRegs r; r.x[2] = key_string(key); return r; }()).x0 & 0xff) != 0;
        std::lock_guard lock(mutex);
        automatic[{cls, key}] = value;
        return value;
    }
    Address subclass_for(Address original) {
        {
            std::lock_guard lock(mutex);
            if (const auto found = kvo_classes.find(original); found != kvo_classes.end()) return found->second;
        }
        const auto name = "NSKVONotifying_" + support.cf.utf8(support.call(foundation_library, "_NSStringFromClass", {original}));
        auto cls = support.objc.find_class(name);
        if (!cls) cls = support.objc.register_native_class(name, original, 16);
        support.method(cls, "class", [original](CpuState& s, GuestMemory&) { s.x[0] = original; }, "#@:");
        support.method(cls, "_isKVOA", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
        std::lock_guard lock(mutex);
        kvo_classes[original] = cls;
        originals[cls] = original;
        return cls;
    }
    // Wraps one mutator of the subclass in a will/did pair.
    enum class Mutator { setter, insert_one, remove_one, replace_one, insert_many, remove_many, replace_many };
    void override_mutator(Address cls, const std::string& selector_text, const std::string& key, Mutator kind) {
        {
            std::lock_guard lock(mutex);
            if (!overridden.insert({cls, selector_text}).second) return;
        }
        const auto selector = support.objc.selector(selector_text);
        const auto key_object = key_string(key);
        // The override keeps the original's type encoding (KVC reads it).
        std::string types = "v@:@";
        if (const auto method = support.call(libobjc, "_class_getInstanceMethod", {support.send_id(cls, "superclass"), selector}))
            if (const auto encoding = support.call(libobjc, "_method_getTypeEncoding", {method})) types = c_string(support.memory(), encoding, 4096);
        support.method(cls, selector_text, [this, cls, selector, key_object, kind](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            CallRegs registers;
            for (unsigned index = 2; index < 8; ++index) registers.x[index] = s.x[index];
            for (unsigned index = 0; index < 8; ++index) registers.v[index] = s.v[index];
            registers.indirect_result = s.x[8];
            Address indexes{};
            std::uint64_t change = kvo::kind_setting;
            switch (kind) {
            case Mutator::setter: break;
            case Mutator::insert_one: change = kvo::kind_insertion; indexes = index_set(s.x[3]); break;
            case Mutator::remove_one: change = kvo::kind_removal; indexes = index_set(s.x[2]); break;
            case Mutator::replace_one: change = kvo::kind_replacement; indexes = index_set(s.x[2]); break;
            case Mutator::insert_many: change = kvo::kind_insertion; indexes = s.x[3]; break;
            case Mutator::remove_many: change = kvo::kind_removal; indexes = s.x[2]; break;
            case Mutator::replace_many: change = kvo::kind_replacement; indexes = s.x[2]; break;
            }
            if (kind == Mutator::setter) static_cast<void>(support.send_id(self, "willChangeValueForKey:", {key_object}));
            else static_cast<void>(support.send_id(self, "willChange:valuesAtIndexes:forKey:", {change, indexes, key_object}));
            const auto result = support.objc.send_super(self, cls, selector, registers);
            if (kind == Mutator::setter) static_cast<void>(support.send_id(self, "didChangeValueForKey:", {key_object}));
            else static_cast<void>(support.send_id(self, "didChange:valuesAtIndexes:forKey:", {change, indexes, key_object}));
            s.x[0] = result.x0;
            s.x[1] = result.x1;
        }, types);
    }
    Address index_set(Address index) {
        return support.send_id(class_named(support, "NSIndexSet"), "indexSetWithIndex:", {index});
    }
    void ensure_automatic(Address object, const std::string& key) {
        if (!object) return;
        const auto view = support.process.objects().peek(object);
        if (!view || view->immortal || view->kind != ObjectKind::instance) return;
        const auto current = support.objc.object_class(object);
        const auto original = original_class(current);
        if (swift_class(original) || !automatic_for(original, key)) return;
        const auto cls = subclass_for(original);
        const auto cap = capitalized(key);
        const auto responds = [&](const std::string& selector) {
            return support.objc.responds(object, support.objc.selector(selector));
        };
        if (responds("set" + cap + ":")) override_mutator(cls, "set" + cap + ":", key, Mutator::setter);
        const std::array<std::pair<std::string, Mutator>, 6> indexed{{
            {"insertObject:in" + cap + "AtIndex:", Mutator::insert_one},
            {"removeObjectFrom" + cap + "AtIndex:", Mutator::remove_one},
            {"replaceObjectIn" + cap + "AtIndex:withObject:", Mutator::replace_one},
            {"insert" + cap + ":atIndexes:", Mutator::insert_many},
            {"remove" + cap + "AtIndexes:", Mutator::remove_many},
            {"replace" + cap + "AtIndexes:with" + cap + ":", Mutator::replace_many}}};
        for (const auto& [selector, kind] : indexed) if (responds(selector)) override_mutator(cls, selector, key, kind);
        if (current != cls) static_cast<void>(support.call(libobjc, "_object_setClass", {object, cls}));
    }

    // ---- dependent keys ----
    std::vector<std::string> affecting(Address object, const std::string& key) {
        const auto cls = original_class(support.objc.object_class(object));
        {
            std::lock_guard lock(mutex);
            if (const auto found = affecting_cache.find({cls, key}); found != affecting_cache.end()) return found->second;
        }
        std::vector<std::string> paths;
        if (const auto set = support.send_id(cls, "keyPathsForValuesAffectingValueForKey:", {key_string(key)})) {
            const auto list = support.send_id(set, "allObjects");
            const auto count = support.send_id(list, "count");
            for (Address index = 0; index < count; ++index) {
                CallRegs r;
                r.x[2] = index;
                paths.push_back(support.cf.utf8(support.send(list, "objectAtIndex:", r).x0));
            }
        }
        std::lock_guard lock(mutex);
        affecting_cache[{cls, key}] = paths;
        return paths;
    }
    kvo::LinkHook hook_for(Address object) {
        std::lock_guard lock(mutex);
        if (link_hooks.empty()) return {};
        const auto cls = support.objc.object_class(object);
        const auto found = link_hooks.find(originals.contains(cls) ? originals[cls] : cls);
        return found == link_hooks.end() ? kvo::LinkHook{} : found->second;
    }

    // ---- links ----
    void detach(const std::shared_ptr<Registration>& reg, std::size_t from) {
        std::vector<std::uint64_t> dependents;
        {
            std::lock_guard lock(mutex);
            for (std::size_t depth = from; depth < reg->objects.size(); ++depth) {
                const auto found = links.find({reg->objects[depth], reg->keys[depth]});
                if (found != links.end()) {
                    auto& list = found->second;
                    std::erase_if(list, [&](const Link& link) { return link.id == reg->id && link.depth == depth; });
                    if (list.empty()) links.erase(found);
                }
                if (depth < reg->dependents.size()) {
                    dependents.insert(dependents.end(), reg->dependents[depth].begin(), reg->dependents[depth].end());
                    reg->dependents[depth].clear();
                }
            }
            if (reg->objects.size() > from) reg->objects.resize(from);
        }
        for (const auto id : dependents) unobserve(id);
    }
    void attach(const std::shared_ptr<Registration>& reg, std::size_t from) {
        detach(reg, from);
        std::vector<Address> chain;
        Address current = from == 0 ? reg->root : 0;
        if (from > 0) {
            Address parent;
            {
                std::lock_guard lock(mutex);
                if (reg->objects.size() < from) return;   // an earlier link is nil
                parent = reg->objects[from - 1];
            }
            current = value_for_key(parent, reg->keys[from - 1]);
        }
        for (std::size_t depth = from; depth < reg->keys.size() && current; ++depth) {
            chain.push_back(current);
            if (depth + 1 < reg->keys.size()) current = value_for_key(current, reg->keys[depth]);
        }
        {
            std::lock_guard lock(mutex);
            if (!reg->alive) return;
            for (std::size_t index = 0; index < chain.size(); ++index) {
                const auto depth = from + index;
                reg->objects.push_back(chain[index]);
                links[{chain[index], reg->keys[depth]}].push_back({reg->id, depth});
            }
            if (reg->dependents.size() < reg->keys.size()) reg->dependents.resize(reg->keys.size());
        }
        for (std::size_t index = 0; index < chain.size(); ++index) {
            const auto depth = from + index;
            const auto object = chain[index];
            const auto& key = reg->keys[depth];
            on_dealloc(object, "kvo", [this](Address gone) { object_deallocated(gone); });
            ensure_automatic(object, key);
            if (const auto hook = hook_for(object)) hook(object, key);
            if (dependent_depth > 8) continue;   // cyclic dependent keys
            ++dependent_depth;
            std::vector<std::uint64_t> ids;
            try {
                for (const auto& path : affecting(object, key)) {
                    if (path == key) continue;
                    ids.push_back(add_internal(object, path, kvo::option_prior, {}, [this, object, key](bool prior) {
                        const auto key_object = key_string(key);
                        static_cast<void>(support.send_id(object, prior ? "willChangeValueForKey:" : "didChangeValueForKey:", {key_object}));
                    }));
                }
            } catch (...) { --dependent_depth; throw; }
            --dependent_depth;
            std::lock_guard lock(mutex);
            if (depth < reg->dependents.size()) reg->dependents[depth] = std::move(ids);
        }
    }

    // ---- registrations ----
    std::shared_ptr<Registration> make(Address root, Address path, std::uint64_t options) {
        auto reg = std::make_shared<Registration>();
        reg->root = root;
        reg->path_string = support.send_id(path, "copy");
        reg->path = support.cf.utf8(path);
        reg->keys = split(reg->path);
        reg->options = options;
        std::lock_guard lock(mutex);
        reg->id = next++;
        registrations[reg->id] = reg;
        return reg;
    }
    std::uint64_t add_internal(Address object, const std::string& path, std::uint64_t options, kvo::Callback callback,
                               std::function<void(bool)> dependent = {}) {
        const auto path_object = support.cf.string(path);
        const auto reg = make(object, path_object, options);
        support.release(path_object);
        reg->callback = std::move(callback);
        reg->dependent = std::move(dependent);
        attach(reg, 0);
        if (options & kvo::option_initial) notify(*reg, initial_change(*reg));
        return reg->id;
    }
    void remove(const std::shared_ptr<Registration>& reg) {
        {
            std::lock_guard lock(mutex);
            if (!reg->alive) return;
            reg->alive = false;
            registrations.erase(reg->id);
        }
        detach(reg, 0);
        support.release(reg->path_string);
    }
    void unobserve(std::uint64_t id) {
        std::shared_ptr<Registration> reg;
        {
            std::lock_guard lock(mutex);
            const auto found = registrations.find(id);
            if (found == registrations.end()) return;
            reg = found->second;
        }
        remove(reg);
    }

    // ---- notification ----
    Address boxed(Address value) { return value ? value : null_object(); }
    Address change_dictionary(std::uint64_t kind, Address new_value, bool has_new, Address old_value, bool has_old, Address indexes, bool prior) {
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const auto add = [&](std::string_view name, Address value) {
            owned.push_back(support.cf.string(name));
            entries.emplace_back(owned.back(), value);
        };
        const auto kind_number = support.cf.integer(static_cast<std::int64_t>(kind), 4);
        add("kind", kind_number);
        if (has_new) add("new", boxed(new_value));
        if (has_old) add("old", boxed(old_value));
        if (indexes) add("indexes", indexes);
        if (prior) add("notificationIsPrior", support.cf.boolean(true));
        const auto result = support.cf.dictionary(entries);
        for (const auto value : owned) support.release(value);
        support.release(kind_number);
        return autoreleased(support, result);
    }
    Address initial_change(const Registration& reg) {
        const bool has_new = reg.options & kvo::option_new;
        return change_dictionary(kvo::kind_setting, has_new ? path_value(reg) : 0, has_new, 0, false, 0, false);
    }
    void notify(const Registration& reg, Address change) {
        if (reg.dependent) {
            const auto prior = support.send_id(change, "objectForKey:", {key_string("notificationIsPrior")});
            reg.dependent(prior != 0);
            return;
        }
        if (reg.callback) { reg.callback(reg.root, change); return; }
        CallRegs registers;
        registers.x[2] = reg.path_string;
        registers.x[3] = reg.root;
        registers.x[4] = change;
        registers.x[5] = reg.context;
        static_cast<void>(support.send(reg.observer, "observeValueForKeyPath:ofObject:change:context:", registers));
    }
    // The values an indexed change touches (+0 array), or the whole path's value.
    Address changed_value(const Registration& reg, std::size_t depth, std::uint64_t kind, Address indexes, Address object) {
        if (kind == kvo::kind_setting || !indexes || depth + 1 != reg.keys.size()) return path_value(reg);
        const auto collection = value_for_key(object, reg.keys[depth]);
        std::vector<Address> values;
        constexpr Address not_found = 0x7fffffffffffffffULL;
        for (auto index = support.send_id(indexes, "firstIndex"); index != not_found;
             index = support.send_id(indexes, "indexGreaterThanIndex:", {index})) {
            if (!collection) break;
            CallRegs r;
            r.x[2] = index;
            if (index < support.send_id(collection, "count")) values.push_back(support.send(collection, "objectAtIndex:", r).x0);
        }
        return array(support, values);
    }
    std::vector<std::pair<std::shared_ptr<Registration>, std::size_t>> linked(Address object, const std::string& key) {
        std::vector<std::pair<std::shared_ptr<Registration>, std::size_t>> result;
        std::lock_guard lock(mutex);
        const auto found = links.find({object, key});
        if (found == links.end()) return result;
        for (const auto& link : found->second)
            if (const auto reg = registrations.find(link.id); reg != registrations.end()) result.emplace_back(reg->second, link.depth);
        return result;
    }
    void will(Address object, const std::string& key, std::uint64_t kind, Address indexes) {
        Pending pending;
        pending.object = object;
        pending.key = key;
        pending.registrations = linked(object, key);
        for (const auto& [reg, depth] : pending.registrations) {
            Address old{};
            const bool prior = reg->options & kvo::option_prior;
            if ((reg->options & kvo::option_old) || prior) old = objc_retain(support, changed_value(*reg, depth, kind, indexes, object));
            if (prior)
                notify(*reg, change_dictionary(kind, 0, false, old, (reg->options & kvo::option_old) != 0, kind == kvo::kind_setting ? 0 : indexes, true));
            pending.old.emplace_back(reg->id, old);
        }
        pending_changes.push_back(std::move(pending));
    }
    void did(Address object, const std::string& key, std::uint64_t kind, Address indexes) {
        Pending pending;
        bool found_pending{};
        for (auto it = pending_changes.rbegin(); it != pending_changes.rend(); ++it) {
            if (it->object != object || it->key != key) continue;
            pending = std::move(*it);
            pending_changes.erase(std::next(it).base());
            found_pending = true;
            break;
        }
        auto targets = found_pending ? pending.registrations : linked(object, key);
        for (const auto& [reg, depth] : targets) {
            bool alive;
            { std::lock_guard lock(mutex); alive = reg->alive; }
            if (alive && depth + 1 < reg->keys.size()) attach(reg, depth + 1);
        }
        for (const auto& [reg, depth] : targets) {
            bool alive;
            { std::lock_guard lock(mutex); alive = reg->alive; }
            if (!alive) continue;
            Address old{};
            for (const auto& [id, value] : pending.old) if (id == reg->id) old = value;
            const bool has_new = reg->options & kvo::option_new, has_old = (reg->options & kvo::option_old) && found_pending;
            const auto new_value = has_new ? changed_value(*reg, depth, kind, indexes, object) : 0;
            notify(*reg, change_dictionary(kind, new_value, has_new, old, has_old, kind == kvo::kind_setting ? 0 : indexes, false));
        }
        for (const auto& [id, value] : pending.old) { static_cast<void>(id); objc_release(support, value); }
    }
};

std::shared_ptr<Kvo> instance;

Kvo& kvo_instance() {
    if (!instance) fail("key-value observing is not installed");
    return *instance;
}

} // namespace

namespace kvo {

std::uint64_t observe(Support& support, Address object, std::string key_path, std::uint64_t options, Callback callback) {
    static_cast<void>(support);
    if (!object) return 0;
    return kvo_instance().add_internal(object, key_path, options, std::move(callback));
}

void unobserve(Support& support, std::uint64_t id) {
    static_cast<void>(support);
    if (id) kvo_instance().unobserve(id);
}

bool notifies_automatically(Support& support, Address object, std::string_view key) {
    static_cast<void>(support);
    if (!instance) return false;
    auto& self = *instance;
    if (self.linked(object, std::string(key)).empty()) return false;
    return self.automatic_for(self.original_class(self.support.objc.object_class(object)), std::string(key));
}

void on_dealloc(Support& support, Address object, std::string_view tag, std::function<void(Address)> cleanup) {
    static_cast<void>(support);
    kvo_instance().on_dealloc(object, tag, std::move(cleanup));
}

void set_link_hook(Support& support, Address cls, LinkHook hook) {
    static_cast<void>(support);
    auto& self = kvo_instance();
    std::lock_guard lock(self.mutex);
    self.link_hooks[cls] = std::move(hook);
}

} // namespace kvo

void install_key_value_observing(Support& support) {
    auto self = std::make_shared<Kvo>(support);
    instance = self;
    const auto root = class_named(support, "NSObject");
    for (const auto& [name, value] : std::array<std::pair<const char*, const char*>, 5>{{
             {"NSKeyValueChangeKindKey", "kind"}, {"NSKeyValueChangeNewKey", "new"}, {"NSKeyValueChangeOldKey", "old"},
             {"NSKeyValueChangeIndexesKey", "indexes"}, {"NSKeyValueChangeNotificationIsPriorKey", "notificationIsPrior"}}})
        if (!support.process.resolve(foundation_library, "_" + std::string(name))) support.string_constant(foundation_library, "_" + std::string(name), value);

    support.method(root, "addObserver:forKeyPath:options:context:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) fail("NSInvalidArgumentException: -addObserver:forKeyPath:options:context: needs an observer and a key path");
        const auto reg = self->make(s.x[0], s.x[3], s.x[4]);
        reg->observer = s.x[2];
        reg->context = s.x[5];
        self->attach(reg, 0);
        if (reg->options & kvo::option_initial) self->notify(*reg, self->initial_change(*reg));
    }, "v@:@@Q^v");
    const auto remove = [self](Address object, Address observer, Address path, std::optional<Address> context) {
        const auto text_path = path ? self->support.cf.utf8(path) : std::string{};
        std::shared_ptr<Registration> match;
        {
            std::lock_guard lock(self->mutex);
            for (auto it = self->registrations.rbegin(); it != self->registrations.rend(); ++it) {
                const auto& reg = *it->second;
                if (reg.root != object || reg.observer != observer || reg.callback || reg.dependent || reg.path != text_path) continue;
                if (context && reg.context != *context) continue;
                match = it->second;
                break;
            }
        }
        if (!match) fail("NSRangeException: Cannot remove an observer for the key path \"" + text_path + "\" because it is not registered as an observer");
        self->remove(match);
    };
    support.method(root, "removeObserver:forKeyPath:", [remove](CpuState& s, GuestMemory&) { remove(s.x[0], s.x[2], s.x[3], std::nullopt); }, "v@:@@");
    support.method(root, "removeObserver:forKeyPath:context:", [remove](CpuState& s, GuestMemory&) { remove(s.x[0], s.x[2], s.x[3], s.x[4]); }, "v@:@@^v");

    const auto key_of = [self](Address key) {
        if (!key) fail("NSInvalidArgumentException: key-value observing change with a nil key");
        return self->support.cf.utf8(key);
    };
    support.method(root, "willChangeValueForKey:", [self, key_of](CpuState& s, GuestMemory&) {
        self->will(s.x[0], key_of(s.x[2]), kvo::kind_setting, 0);
    }, "v@:@");
    support.method(root, "didChangeValueForKey:", [self, key_of](CpuState& s, GuestMemory&) {
        self->did(s.x[0], key_of(s.x[2]), kvo::kind_setting, 0);
    }, "v@:@");
    support.method(root, "willChange:valuesAtIndexes:forKey:", [self, key_of](CpuState& s, GuestMemory&) {
        self->will(s.x[0], key_of(s.x[4]), s.x[2], s.x[3]);
    }, "v@:Q@@");
    support.method(root, "didChange:valuesAtIndexes:forKey:", [self, key_of](CpuState& s, GuestMemory&) {
        self->did(s.x[0], key_of(s.x[4]), s.x[2], s.x[3]);
    }, "v@:Q@@");
    // Set mutations report a setting change of the key.
    support.method(root, "willChangeValueForKey:withSetMutation:usingObjects:", [self, key_of](CpuState& s, GuestMemory&) {
        self->will(s.x[0], key_of(s.x[2]), kvo::kind_setting, 0);
    }, "v@:@Q@");
    support.method(root, "didChangeValueForKey:withSetMutation:usingObjects:", [self, key_of](CpuState& s, GuestMemory&) {
        self->did(s.x[0], key_of(s.x[2]), kvo::kind_setting, 0);
    }, "v@:@Q@");
    support.method(root, "automaticallyNotifiesObserversForKey:", [self](CpuState& s, GuestMemory&) {
        const auto key = s.x[2] ? self->support.cf.utf8(s.x[2]) : std::string{};
        const auto specific = "automaticallyNotifiesObserversOf" + capitalized(key);
        if (!key.empty() && self->support.objc.responds(s.x[0], self->support.objc.selector(specific))) {
            s.x[0] = self->support.send(s.x[0], specific).x0 & 0xff;
            return;
        }
        s.x[0] = 1;
    }, "B@:@", true);
    support.method(root, "keyPathsForValuesAffectingValueForKey:", [self](CpuState& s, GuestMemory&) {
        const auto key = s.x[2] ? self->support.cf.utf8(s.x[2]) : std::string{};
        const auto specific = "keyPathsForValuesAffecting" + capitalized(key);
        if (!key.empty() && self->support.objc.responds(s.x[0], self->support.objc.selector(specific))) {
            s.x[0] = self->support.send(s.x[0], specific).x0;
            return;
        }
        s.x[0] = self->support.send_id(class_named(self->support, "NSSet"), "set");
    }, "@@:@", true);
    support.method(root, "observeValueForKeyPath:ofObject:change:context:", [self](CpuState& s, GuestMemory&) {
        fail("NSInternalInconsistencyException: " + self->support.cf.utf8(self->support.call(foundation_library, "_NSStringFromClass",
             {self->support.objc.object_class(s.x[0])})) + ": an -observeValueForKeyPath:ofObject:change:context: message was received but not handled (key path " +
             (s.x[2] ? self->support.cf.utf8(s.x[2]) : std::string("nil")) + ")");
    }, "v@:@@@^v");
    support.method(root, "observationInfo", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex);
        const auto found = self->observation_info.find(s.x[0]);
        s.x[0] = found == self->observation_info.end() ? 0 : found->second;
    }, "^v@:");
    support.method(root, "setObservationInfo:", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex);
        if (s.x[2]) self->observation_info[s.x[0]] = s.x[2]; else self->observation_info.erase(s.x[0]);
    }, "v@:^v");
}

} // namespace alchemy::gui::completion_kit
