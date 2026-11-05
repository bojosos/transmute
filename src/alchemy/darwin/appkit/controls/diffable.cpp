#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Diffable data sources: NSDiffableDataSourceSnapshot and the collection
// and table view data sources over it.
// * A snapshot holds section identifiers and, in each, item identifiers
//   (retained, compared with -isEqual:); the documented building, querying,
//   inserting, deleting, moving and reloading calls edit it. Duplicate
//   identifiers, and identifiers that are not in it, raise as AppKit does.
// * NSCollectionViewDiffableDataSource becomes its collection view's data
//   source: sections and items come from the applied snapshot, items from
//   the item provider block, supplementary views from the supplementary
//   view provider.
// * NSTableViewDiffableDataSource becomes its table view's data source:
//   rows are the snapshot's items in order (with a header row before each
//   section when there is a section header view provider; those rows are
//   group rows), cell views from the cell provider, row views from the row
//   view provider.
// * Applying a snapshot reloads the view: differences are not animated
//   (animatingDifferences is accepted; the completion handler runs after
//   the reload).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

struct Snapshot {
    struct Section {
        Address identifier{};                // +1
        std::vector<Address> items;          // +1
    };
    std::vector<Section> sections;
    std::vector<Address> reloaded_items, reloaded_sections;   // +1
};
struct SnapshotState final : ObjectPayload { Snapshot snapshot; };
struct SourceState final : ObjectPayload {
    Address view{};                          // the collection / table view (not retained, as AppKit's weak reference)
    Address provider{};                      // +1 block: the item / cell provider
    Address supplementary_provider{};        // +1 block (collection) or the section header view provider (table)
    Address row_provider{};                  // +1 block (table row views)
    Snapshot snapshot;                       // the applied snapshot
    std::int64_t default_animation{};
    bool table{};
};

[[noreturn]] void inconsistency(const std::string& what) {
    throw std::invalid_argument("NSInternalInconsistencyException: " + what);
}

std::shared_ptr<SnapshotState> snapshot_of(AppKit& kit, Address object) { return kit.objc.host_state<SnapshotState>(object); }
std::shared_ptr<SourceState> source_of(AppKit& kit, Address object) { return kit.objc.host_state<SourceState>(object); }

bool same(AppKit& kit, Address a, Address b) { return a == b || (a && b && kit.send_bool(a, "isEqual:", {b})); }

std::optional<std::size_t> find_section(AppKit& kit, const Snapshot& snapshot, Address identifier) {
    for (std::size_t i = 0; i < snapshot.sections.size(); ++i)
        if (same(kit, snapshot.sections[i].identifier, identifier)) return i;
    return std::nullopt;
}
struct ItemPlace { std::size_t section{}, index{}; };
std::optional<ItemPlace> find_item(AppKit& kit, const Snapshot& snapshot, Address identifier) {
    for (std::size_t s = 0; s < snapshot.sections.size(); ++s)
        for (std::size_t i = 0; i < snapshot.sections[s].items.size(); ++i)
            if (same(kit, snapshot.sections[s].items[i], identifier)) return ItemPlace{s, i};
    return std::nullopt;
}

Snapshot copy_of(AppKit& kit, const Snapshot& from) {
    Snapshot out;
    for (const auto& section : from.sections) {
        Snapshot::Section copy;
        copy.identifier = kit.retain(section.identifier);
        for (const auto item : section.items) copy.items.push_back(kit.retain(item));
        out.sections.push_back(std::move(copy));
    }
    for (const auto item : from.reloaded_items) out.reloaded_items.push_back(kit.retain(item));
    for (const auto item : from.reloaded_sections) out.reloaded_sections.push_back(kit.retain(item));
    return out;
}
void clear(AppKit& kit, Snapshot& snapshot) {
    for (auto& section : snapshot.sections) {
        kit.release(section.identifier);
        for (const auto item : section.items) kit.release(item);
    }
    snapshot.sections.clear();
    for (const auto item : std::exchange(snapshot.reloaded_items, {})) kit.release(item);
    for (const auto item : std::exchange(snapshot.reloaded_sections, {})) kit.release(item);
}

std::size_t item_count(const Snapshot& snapshot) {
    std::size_t count = 0;
    for (const auto& section : snapshot.sections) count += section.items.size();
    return count;
}

// New identifiers must be unique in the snapshot and among themselves.
std::vector<Address> fresh_items(AppKit& kit, const Snapshot& snapshot, Address array) {
    auto items = array ? kit.items(array) : std::vector<Address>{};
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (find_item(kit, snapshot, items[i])) inconsistency("an item identifier is already in the snapshot");
        for (std::size_t j = 0; j < i; ++j) if (same(kit, items[i], items[j])) inconsistency("duplicate item identifiers");
    }
    return items;
}
std::vector<Address> fresh_sections(AppKit& kit, const Snapshot& snapshot, Address array) {
    auto sections = array ? kit.items(array) : std::vector<Address>{};
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (find_section(kit, snapshot, sections[i])) inconsistency("a section identifier is already in the snapshot");
        for (std::size_t j = 0; j < i; ++j) if (same(kit, sections[i], sections[j])) inconsistency("duplicate section identifiers");
    }
    return sections;
}

// The flat row of an item in a table source (section header rows counted when there is a header provider).
struct Row { std::optional<std::size_t> section; std::optional<ItemPlace> item; };
std::vector<Row> table_rows(const SourceState& source) {
    std::vector<Row> rows;
    for (std::size_t s = 0; s < source.snapshot.sections.size(); ++s) {
        if (source.supplementary_provider) rows.push_back({s, std::nullopt});
        for (std::size_t i = 0; i < source.snapshot.sections[s].items.size(); ++i) rows.push_back({std::nullopt, ItemPlace{s, i}});
    }
    return rows;
}

} // namespace

void install_diffable_data_sources(AppKit& kit) {
    auto* const k = &kit;
    const auto copy_block = [k](Address block) { return block ? k->guest_call(libsystem_library, "__Block_copy", {block}) : 0; };
    const auto release_block = [k](Address block) { if (block) static_cast<void>(k->guest_call(libsystem_library, "__Block_release", {block})); };

    // ---- NSDiffableDataSourceSnapshot ----
    const auto snap = kit.declare_class("NSDiffableDataSourceSnapshot", "NSObject");
    kit.method(snap, "dealloc", [k, snap](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<SnapshotState>(s.x[0])) clear(*k, state->snapshot);
        static_cast<void>(k->objc.send_super(s.x[0], snap, k->sel("dealloc")));
    }, "v@:");
    kit.method(snap, "copyWithZone:", [k](CpuState& s, GuestMemory&) {
        const auto made = k->alloc_init(k->objc.object_class(s.x[0]));
        snapshot_of(*k, made)->snapshot = copy_of(*k, snapshot_of(*k, s.x[0])->snapshot);
        s.x[0] = made;
    }, "@@:^v");
    kit.method(snap, "numberOfItems", [k](CpuState& s, GuestMemory&) { s.x[0] = item_count(snapshot_of(*k, s.x[0])->snapshot); }, "q@:");
    kit.method(snap, "numberOfSections", [k](CpuState& s, GuestMemory&) { s.x[0] = snapshot_of(*k, s.x[0])->snapshot.sections.size(); }, "q@:");
    kit.method(snap, "sectionIdentifiers", [k](CpuState& s, GuestMemory&) {
        std::vector<Address> out;
        for (const auto& section : snapshot_of(*k, s.x[0])->snapshot.sections) out.push_back(section.identifier);
        s.x[0] = k->auto_array(out);
    });
    kit.method(snap, "itemIdentifiers", [k](CpuState& s, GuestMemory&) {
        std::vector<Address> out;
        for (const auto& section : snapshot_of(*k, s.x[0])->snapshot.sections) out.insert(out.end(), section.items.begin(), section.items.end());
        s.x[0] = k->auto_array(out);
    });
    kit.method(snap, "numberOfItemsInSection:", [k](CpuState& s, GuestMemory&) {
        const auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        const auto found = find_section(*k, snapshot, s.x[2]);
        if (!found) inconsistency("numberOfItemsInSection: with a section identifier not in the snapshot");
        s.x[0] = snapshot.sections[*found].items.size();
    }, "q@:@");
    kit.method(snap, "itemIdentifiersInSectionWithIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        const auto found = find_section(*k, snapshot, s.x[2]);
        if (!found) inconsistency("itemIdentifiersInSectionWithIdentifier: with a section identifier not in the snapshot");
        s.x[0] = k->auto_array(snapshot.sections[*found].items);
    });
    kit.method(snap, "sectionIdentifierForSectionContainingItemIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        const auto found = find_item(*k, snapshot, s.x[2]);
        s.x[0] = found ? snapshot.sections[found->section].identifier : 0;
    });
    kit.method(snap, "indexOfItemIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        const auto found = find_item(*k, snapshot, s.x[2]);
        if (!found) { s.x[0] = static_cast<Address>(not_found); return; }
        std::size_t flat = found->index;
        for (std::size_t i = 0; i < found->section; ++i) flat += snapshot.sections[i].items.size();
        s.x[0] = flat;
    }, "q@:@");
    kit.method(snap, "indexOfSectionIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto found = find_section(*k, snapshot_of(*k, s.x[0])->snapshot, s.x[2]);
        s.x[0] = found ? *found : static_cast<Address>(not_found);
    }, "q@:@");
    // Building.
    kit.method(snap, "appendSectionsWithIdentifiers:", [k](CpuState& s, GuestMemory&) {
        auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        for (const auto identifier : fresh_sections(*k, snapshot, s.x[2])) snapshot.sections.push_back({k->retain(identifier), {}});
    }, "v@:@");
    kit.method(snap, "appendItemsWithIdentifiers:", [k](CpuState& s, GuestMemory&) {
        auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        if (snapshot.sections.empty()) inconsistency("appendItemsWithIdentifiers: with no sections in the snapshot");
        for (const auto identifier : fresh_items(*k, snapshot, s.x[2])) snapshot.sections.back().items.push_back(k->retain(identifier));
    }, "v@:@");
    kit.method(snap, "appendItemsWithIdentifiers:intoSectionWithIdentifier:", [k](CpuState& s, GuestMemory&) {
        auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        const auto section = find_section(*k, snapshot, s.x[3]);
        if (!section) inconsistency("appendItemsWithIdentifiers:intoSectionWithIdentifier: with a section not in the snapshot");
        for (const auto identifier : fresh_items(*k, snapshot, s.x[2])) snapshot.sections[*section].items.push_back(k->retain(identifier));
    }, "v@:@@");
    for (const bool after : {false, true}) {
        kit.method(snap, after ? "insertItemsWithIdentifiers:afterItemWithIdentifier:" : "insertItemsWithIdentifiers:beforeItemWithIdentifier:",
                   [k, after](CpuState& s, GuestMemory&) {
            auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
            const auto place = find_item(*k, snapshot, s.x[3]);
            if (!place) inconsistency("inserting items next to an item identifier not in the snapshot");
            auto items = fresh_items(*k, snapshot, s.x[2]);
            for (const auto item : items) k->retain(item);
            auto& list = snapshot.sections[place->section].items;
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(place->index + (after ? 1 : 0)), items.begin(), items.end());
        }, "v@:@@");
        kit.method(snap, after ? "insertSectionsWithIdentifiers:afterSectionWithIdentifier:" : "insertSectionsWithIdentifiers:beforeSectionWithIdentifier:",
                   [k, after](CpuState& s, GuestMemory&) {
            auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
            const auto place = find_section(*k, snapshot, s.x[3]);
            if (!place) inconsistency("inserting sections next to a section identifier not in the snapshot");
            std::vector<Snapshot::Section> added;
            for (const auto identifier : fresh_sections(*k, snapshot, s.x[2])) added.push_back({k->retain(identifier), {}});
            snapshot.sections.insert(snapshot.sections.begin() + static_cast<std::ptrdiff_t>(*place + (after ? 1 : 0)), added.begin(), added.end());
        }, "v@:@@");
        kit.method(snap, after ? "moveItemWithIdentifier:afterItemWithIdentifier:" : "moveItemWithIdentifier:beforeItemWithIdentifier:",
                   [k, after](CpuState& s, GuestMemory&) {
            auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
            const auto from = find_item(*k, snapshot, s.x[2]);
            if (!from || !find_item(*k, snapshot, s.x[3])) inconsistency("moving an item identifier not in the snapshot");
            if (same(*k, s.x[2], s.x[3])) return;
            const auto moved = snapshot.sections[from->section].items[from->index];
            snapshot.sections[from->section].items.erase(snapshot.sections[from->section].items.begin() + static_cast<std::ptrdiff_t>(from->index));
            const auto to = *find_item(*k, snapshot, s.x[3]);
            auto& list = snapshot.sections[to.section].items;
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(to.index + (after ? 1 : 0)), moved);
        }, "v@:@@");
        kit.method(snap, after ? "moveSectionWithIdentifier:afterSectionWithIdentifier:" : "moveSectionWithIdentifier:beforeSectionWithIdentifier:",
                   [k, after](CpuState& s, GuestMemory&) {
            auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
            const auto from = find_section(*k, snapshot, s.x[2]);
            if (!from || !find_section(*k, snapshot, s.x[3])) inconsistency("moving a section identifier not in the snapshot");
            if (same(*k, s.x[2], s.x[3])) return;
            auto moved = std::move(snapshot.sections[*from]);
            snapshot.sections.erase(snapshot.sections.begin() + static_cast<std::ptrdiff_t>(*from));
            const auto to = *find_section(*k, snapshot, s.x[3]);
            snapshot.sections.insert(snapshot.sections.begin() + static_cast<std::ptrdiff_t>(to + (after ? 1 : 0)), std::move(moved));
        }, "v@:@@");
    }
    kit.method(snap, "deleteItemsWithIdentifiers:", [k](CpuState& s, GuestMemory&) {
        auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        for (const auto identifier : s.x[2] ? k->items(s.x[2]) : std::vector<Address>{})
            if (const auto place = find_item(*k, snapshot, identifier)) {
                auto& list = snapshot.sections[place->section].items;
                k->release(list[place->index]);
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(place->index));
            }
    }, "v@:@");
    kit.method(snap, "deleteSectionsWithIdentifiers:", [k](CpuState& s, GuestMemory&) {
        auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
        for (const auto identifier : s.x[2] ? k->items(s.x[2]) : std::vector<Address>{})
            if (const auto place = find_section(*k, snapshot, identifier)) {
                auto& section = snapshot.sections[*place];
                k->release(section.identifier);
                for (const auto item : section.items) k->release(item);
                snapshot.sections.erase(snapshot.sections.begin() + static_cast<std::ptrdiff_t>(*place));
            }
    }, "v@:@");
    kit.method(snap, "deleteAllItems", [k](CpuState& s, GuestMemory&) { clear(*k, snapshot_of(*k, s.x[0])->snapshot); }, "v@:");
    // Reloads mark the identifiers (the whole view reloads when the snapshot is applied).
    for (const bool sections : {false, true})
        kit.method(snap, sections ? "reloadSectionsWithIdentifiers:" : "reloadItemsWithIdentifiers:", [k, sections](CpuState& s, GuestMemory&) {
            auto& snapshot = snapshot_of(*k, s.x[0])->snapshot;
            for (const auto identifier : s.x[2] ? k->items(s.x[2]) : std::vector<Address>{}) {
                if (sections ? !find_section(*k, snapshot, identifier) : !find_item(*k, snapshot, identifier))
                    inconsistency(std::string("reloading ") + (sections ? "a section" : "an item") + " identifier not in the snapshot");
                (sections ? snapshot.reloaded_sections : snapshot.reloaded_items).push_back(k->retain(identifier));
            }
        }, "v@:@");

    // ---- NSCollectionViewDiffableDataSource ----
    const auto collection_source = kit.declare_class("NSCollectionViewDiffableDataSource", "NSObject");
    const auto source_dealloc = [k, release_block](Address cls) {
        k->method(cls, "dealloc", [k, cls, release_block](CpuState& s, GuestMemory&) {
            if (const auto state = k->objc.find_host_state<SourceState>(s.x[0])) {
                release_block(std::exchange(state->provider, 0));
                release_block(std::exchange(state->supplementary_provider, 0));
                release_block(std::exchange(state->row_provider, 0));
                clear(*k, state->snapshot);
            }
            static_cast<void>(k->objc.send_super(s.x[0], cls, k->sel("dealloc")));
        }, "v@:");
    };
    source_dealloc(collection_source);
    kit.method(collection_source, "initWithCollectionView:itemProvider:", [k, collection_source, copy_block](CpuState& s, GuestMemory&) {
        const auto self = k->objc.send_super(s.x[0], collection_source, k->sel("init")).x0;
        s.x[0] = self;
        if (!self) return;
        if (!s.x[3]) inconsistency("NSCollectionViewDiffableDataSource without an item provider");
        const auto state = source_of(*k, self);
        state->view = s.x[2];
        state->provider = copy_block(s.x[3]);
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "setDataSource:", {self}));
    }, "@@:@@?");
    // Shared by both sources.
    const auto common = [k, copy_block, release_block](Address cls) {
        k->method(cls, "snapshot", [k](CpuState& s, GuestMemory&) {
            const auto made = k->alloc_init(k->objc.find_class("NSDiffableDataSourceSnapshot"));
            snapshot_of(*k, made)->snapshot = copy_of(*k, source_of(*k, s.x[0])->snapshot);
            s.x[0] = k->autorelease(made);
        });
        const auto apply = [k](Address self, Address snapshot_object, Address completion) {
            const auto state = source_of(*k, self);
            auto incoming = snapshot_object ? copy_of(*k, snapshot_of(*k, snapshot_object)->snapshot) : Snapshot{};
            for (const auto item : std::exchange(incoming.reloaded_items, {})) k->release(item);
            for (const auto item : std::exchange(incoming.reloaded_sections, {})) k->release(item);
            clear(*k, state->snapshot);
            state->snapshot = std::move(incoming);
            if (state->view) static_cast<void>(k->send(state->view, "reloadData"));
            if (completion) static_cast<void>(k->invoke_block(completion, {}));
        };
        k->method(cls, "applySnapshot:animatingDifferences:", [apply](CpuState& s, GuestMemory&) { apply(s.x[0], s.x[2], 0); }, "v@:@B");
        k->method(cls, "applySnapshot:animatingDifferences:completion:", [apply](CpuState& s, GuestMemory&) { apply(s.x[0], s.x[2], s.x[4]); }, "v@:@B@?");
        k->method(cls, "supplementaryViewProvider", [k](CpuState& s, GuestMemory&) { s.x[0] = source_of(*k, s.x[0])->supplementary_provider; }, "@?@:");
        k->method(cls, "setSupplementaryViewProvider:", [k, copy_block, release_block](CpuState& s, GuestMemory&) {
            const auto state = source_of(*k, s.x[0]);
            release_block(std::exchange(state->supplementary_provider, copy_block(s.x[2])));
        }, "v@:@?");
    };
    common(collection_source);
    kit.method(collection_source, "itemIdentifierForIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto state = source_of(*k, s.x[0]);
        s.x[0] = 0;
        if (!s.x[2]) return;
        const auto section = static_cast<std::size_t>(k->send_id(s.x[2], "section")), item = static_cast<std::size_t>(k->send_id(s.x[2], "item"));
        if (section < state->snapshot.sections.size() && item < state->snapshot.sections[section].items.size())
            s.x[0] = state->snapshot.sections[section].items[item];
    });
    kit.method(collection_source, "indexPathForItemIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto place = find_item(*k, source_of(*k, s.x[0])->snapshot, s.x[2]);
        s.x[0] = place ? k->send_id(k->objc.find_class("NSIndexPath"), "indexPathForItem:inSection:", {place->index, place->section}) : 0;
    });
    // The data source protocol.
    kit.method(collection_source, "numberOfSectionsInCollectionView:", [k](CpuState& s, GuestMemory&) {
        s.x[0] = source_of(*k, s.x[0])->snapshot.sections.size();
    }, "q@:@");
    kit.method(collection_source, "collectionView:numberOfItemsInSection:", [k](CpuState& s, GuestMemory&) {
        const auto& sections = source_of(*k, s.x[0])->snapshot.sections;
        s.x[0] = s.x[3] < sections.size() ? sections[s.x[3]].items.size() : 0;
    }, "q@:@q");
    kit.method(collection_source, "collectionView:itemForRepresentedObjectAtIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto identifier = k->send_id(self, "itemIdentifierForIndexPath:", {s.x[3]});
        const auto made = k->invoke_block(source_of(*k, self)->provider, {s.x[2], s.x[3], identifier});
        if (!made) inconsistency("the item provider answered nil");
        s.x[0] = made;
    });
    kit.method(collection_source, "collectionView:viewForSupplementaryElementOfKind:atIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto provider = source_of(*k, s.x[0])->supplementary_provider;
        s.x[0] = provider ? k->invoke_block(provider, {s.x[2], s.x[3], s.x[4]}) : 0;
    });

    // ---- NSTableViewDiffableDataSource ----
    const auto table_source = kit.declare_class("NSTableViewDiffableDataSource", "NSObject");
    source_dealloc(table_source);
    kit.method(table_source, "initWithTableView:cellProvider:", [k, table_source, copy_block](CpuState& s, GuestMemory&) {
        const auto self = k->objc.send_super(s.x[0], table_source, k->sel("init")).x0;
        s.x[0] = self;
        if (!self) return;
        if (!s.x[3]) inconsistency("NSTableViewDiffableDataSource without a cell provider");
        const auto state = source_of(*k, self);
        state->table = true;
        state->view = s.x[2];
        state->provider = copy_block(s.x[3]);
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "setDataSource:", {self}));
    }, "@@:@@?");
    common(table_source);
    kit.method(table_source, "sectionHeaderViewProvider", [k](CpuState& s, GuestMemory&) { s.x[0] = source_of(*k, s.x[0])->supplementary_provider; }, "@?@:");
    kit.method(table_source, "setSectionHeaderViewProvider:", [k](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[0], "setSupplementaryViewProvider:", {s.x[2]}));
    }, "v@:@?");
    kit.method(table_source, "rowViewProvider", [k](CpuState& s, GuestMemory&) { s.x[0] = source_of(*k, s.x[0])->row_provider; }, "@?@:");
    kit.method(table_source, "setRowViewProvider:", [k, copy_block, release_block](CpuState& s, GuestMemory&) {
        const auto state = source_of(*k, s.x[0]);
        release_block(std::exchange(state->row_provider, copy_block(s.x[2])));
    }, "v@:@?");
    kit.method(table_source, "defaultRowAnimation", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(source_of(*k, s.x[0])->default_animation); }, "Q@:");
    kit.method(table_source, "setDefaultRowAnimation:", [k](CpuState& s, GuestMemory&) { source_of(*k, s.x[0])->default_animation = static_cast<std::int64_t>(s.x[2]); }, "v@:Q");
    const auto row_at = [k](Address self, std::int64_t row) -> std::optional<Row> {
        const auto rows = table_rows(*source_of(*k, self));
        if (row < 0 || row >= static_cast<std::int64_t>(rows.size())) return std::nullopt;
        return rows[static_cast<std::size_t>(row)];
    };
    kit.method(table_source, "itemIdentifierForRow:", [k, row_at](CpuState& s, GuestMemory&) {
        const auto row = row_at(s.x[0], static_cast<std::int64_t>(s.x[2]));
        s.x[0] = row && row->item ? source_of(*k, s.x[0])->snapshot.sections[row->item->section].items[row->item->index] : 0;
    }, "@@:q");
    kit.method(table_source, "sectionIdentifierForRow:", [k, row_at](CpuState& s, GuestMemory&) {
        const auto row = row_at(s.x[0], static_cast<std::int64_t>(s.x[2]));
        const auto section = row ? (row->section ? *row->section : row->item->section) : 0;
        s.x[0] = row ? source_of(*k, s.x[0])->snapshot.sections[section].identifier : 0;
    }, "@@:q");
    for (const bool sections : {false, true})
        kit.method(table_source, sections ? "rowForSectionIdentifier:" : "rowForItemIdentifier:", [k, sections](CpuState& s, GuestMemory&) {
            const auto state = source_of(*k, s.x[0]);
            const auto rows = table_rows(*state);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& row = rows[i];
                // A section's row: its header row, else its first item's row.
                const bool match = sections ? ((row.section && same(*k, state->snapshot.sections[*row.section].identifier, s.x[2])) ||
                                               (!state->supplementary_provider && row.item && row.item->index == 0 &&
                                                same(*k, state->snapshot.sections[row.item->section].identifier, s.x[2])))
                                            : (row.item && same(*k, state->snapshot.sections[row.item->section].items[row.item->index], s.x[2]));
                if (match) { s.x[0] = i; return; }
            }
            s.x[0] = static_cast<Address>(-1);
        }, "q@:@");
    // The table's data source protocol (views come from the table asking its data source after its delegate).
    kit.method(table_source, "numberOfRowsInTableView:", [k](CpuState& s, GuestMemory&) { s.x[0] = table_rows(*source_of(*k, s.x[0])).size(); }, "q@:@");
    kit.method(table_source, "tableView:viewForTableColumn:row:", [k, row_at](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = source_of(*k, self);
        const auto row = row_at(self, static_cast<std::int64_t>(s.x[4]));
        s.x[0] = 0;
        if (!row) return;
        if (row->section) {
            if (state->supplementary_provider)
                s.x[0] = k->invoke_block(state->supplementary_provider, {s.x[2], s.x[4], state->snapshot.sections[*row->section].identifier});
            return;
        }
        s.x[0] = k->invoke_block(state->provider, {s.x[2], s.x[3], s.x[4], state->snapshot.sections[row->item->section].items[row->item->index]});
    }, "@@:@@q");
    kit.method(table_source, "tableView:rowViewForRow:", [k, row_at](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = source_of(*k, self);
        const auto row = row_at(self, static_cast<std::int64_t>(s.x[3]));
        s.x[0] = 0;
        if (!row || !state->row_provider) return;
        const auto identifier = row->item ? state->snapshot.sections[row->item->section].items[row->item->index]
                                          : state->snapshot.sections[*row->section].identifier;
        s.x[0] = k->invoke_block(state->row_provider, {s.x[2], s.x[3], identifier});
    }, "@@:@q");
    kit.method(table_source, "tableView:isGroupRow:", [row_at](CpuState& s, GuestMemory&) {
        const auto row = row_at(s.x[0], static_cast<std::int64_t>(s.x[3]));
        return_bool(s, row && row->section.has_value());
    }, "B@:@q");
}

} // namespace alchemy::gui::completion_kit::controls
