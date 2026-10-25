// CoreFoundation collections (cf_collections.cpp, the CFSet additions of
// cf_sets_notifications.cpp, the CFCharacterSet constructors of charset.cpp)
// over the full provider set (fill_fixture.hpp). Guest callbacks are CF
// functions themselves: CFStringCompare as the comparator, CFStringAppend as
// the applier (value = a mutable string, context = what to append), and
// CFRetain/CFRelease as context retain and release.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <vector>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr Address not_found = static_cast<Address>(-1);
bool yes(Address value) { return (value & 0xff) != 0; }

Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }
Address mutable_string(Fixture& f, std::string_view text) { return f.cf.mutable_copy(f.cf.string(text)); }
Address values(Fixture& f, std::initializer_list<Address> items) {
    const auto block = f.block(items.size() * 8 + 8);
    std::size_t index = 0;
    for (const auto item : items) f.engine.write64(block + 8 * index++, item);
    return block;
}

void binary_heaps(Fixture& f) {
    const auto callbacks = f.symbol(cf, "_kCFStringBinaryHeapCallBacks");
    const auto heap = f.call(cf, "_CFBinaryHeapCreate", {0, 0, callbacks, 0});
    require(f.call(cf, "_CFGetTypeID", {heap}) == f.call(cf, "_CFBinaryHeapGetTypeID"), "CFBinaryHeap type id");
    const auto out = f.block(8);
    require(!yes(f.call(cf, "_CFBinaryHeapGetMinimumIfPresent", {heap, out})), "an empty heap has no minimum");
    for (const auto text : {"pear", "apple", "fig", "banana", "apple"}) static_cast<void>(f.call(cf, "_CFBinaryHeapAddValue", {heap, string(f, text)}));
    require(f.call(cf, "_CFBinaryHeapGetCount", {heap}) == 5, "CFBinaryHeapGetCount");
    require(yes(f.call(cf, "_CFBinaryHeapGetMinimumIfPresent", {heap, out})) && f.cf.utf8(f.engine.read64(out)) == "apple", "minimum");
    require(f.call(cf, "_CFBinaryHeapGetCountOfValue", {heap, string(f, "apple")}) == 2 &&
                yes(f.call(cf, "_CFBinaryHeapContainsValue", {heap, string(f, "fig")})) &&
                !yes(f.call(cf, "_CFBinaryHeapContainsValue", {heap, string(f, "kiwi")})), "count and membership by compare");
    const auto sorted = f.block(40);
    static_cast<void>(f.call(cf, "_CFBinaryHeapGetValues", {heap, sorted}));
    std::vector<std::string> order;
    for (int index = 0; index < 5; ++index) order.push_back(f.cf.utf8(f.engine.read64(sorted + 8 * index)));
    require(order == std::vector<std::string>{"apple", "apple", "banana", "fig", "pear"}, "CFBinaryHeapGetValues in ascending order");
    static_cast<void>(f.call(cf, "_CFBinaryHeapRemoveMinimumValue", {heap}));
    static_cast<void>(f.call(cf, "_CFBinaryHeapRemoveMinimumValue", {heap}));
    require(f.cf.utf8(f.call(cf, "_CFBinaryHeapGetMinimum", {heap})) == "banana", "remove the minimum twice");
    const auto copy = f.call(cf, "_CFBinaryHeapCreateCopy", {0, 0, heap});
    static_cast<void>(f.call(cf, "_CFBinaryHeapRemoveAllValues", {heap}));
    require(f.call(cf, "_CFBinaryHeapGetCount", {heap}) == 0 && f.call(cf, "_CFBinaryHeapGetCount", {copy}) == 3, "copies are independent");
    // Values retained by the callbacks.
    const auto held = string(f, "held");
    const auto before = f.call(cf, "_CFGetRetainCount", {held});
    static_cast<void>(f.call(cf, "_CFBinaryHeapAddValue", {heap, held}));
    require(f.call(cf, "_CFGetRetainCount", {held}) == before + 1, "the heap retains its values");
    static_cast<void>(f.call(cf, "_CFBinaryHeapRemoveAllValues", {heap}));
    require(f.call(cf, "_CFGetRetainCount", {held}) == before, "and releases them");
    // Without callbacks: pointers, ordered by value.
    const auto raw = f.call(cf, "_CFBinaryHeapCreate", {0, 0, 0, 0});
    for (const Address value : {30, 10, 20}) static_cast<void>(f.call(cf, "_CFBinaryHeapAddValue", {raw, value}));
    require(f.call(cf, "_CFBinaryHeapGetMinimum", {raw}) == 10, "pointer heap");
}

void bit_vectors(Fixture& f) {
    const auto bytes = f.block(2);
    f.memory().write8(bytes, 0xa0);       // 1010 0000
    f.memory().write8(bytes + 1, 0x01);   // 0000 0001
    const auto bits = f.call(cf, "_CFBitVectorCreate", {0, bytes, 16});
    require(f.call(cf, "_CFGetTypeID", {bits}) == f.call(cf, "_CFBitVectorGetTypeID"), "CFBitVector type id");
    require(f.call(cf, "_CFBitVectorGetCount", {bits}) == 16, "count");
    require(f.call(cf, "_CFBitVectorGetBitAtIndex", {bits, 0}) == 1 && f.call(cf, "_CFBitVectorGetBitAtIndex", {bits, 1}) == 0 &&
                f.call(cf, "_CFBitVectorGetBitAtIndex", {bits, 2}) == 1 && f.call(cf, "_CFBitVectorGetBitAtIndex", {bits, 15}) == 1,
            "bits are numbered from the high bit of the first byte");
    require(f.call(cf, "_CFBitVectorGetCountOfBit", {bits, 0, 16, 1}) == 3 && yes(f.call(cf, "_CFBitVectorContainsBit", {bits, 3, 12, 0})) &&
                !yes(f.call(cf, "_CFBitVectorContainsBit", {bits, 3, 12, 1})), "counting and searching ranges");
    require(f.call(cf, "_CFBitVectorGetFirstIndexOfBit", {bits, 1, 15, 1}) == 2 && f.call(cf, "_CFBitVectorGetLastIndexOfBit", {bits, 0, 15, 1}) == 2 &&
                f.call(cf, "_CFBitVectorGetFirstIndexOfBit", {bits, 3, 12, 1}) == not_found, "first and last index of a bit");
    const auto mutable_bits = f.call(cf, "_CFBitVectorCreateMutableCopy", {0, 0, bits});
    static_cast<void>(f.call(cf, "_CFBitVectorSetCount", {mutable_bits, 20}));
    static_cast<void>(f.call(cf, "_CFBitVectorSetBits", {mutable_bits, 16, 4, 1}));
    static_cast<void>(f.call(cf, "_CFBitVectorFlipBitAtIndex", {mutable_bits, 0}));
    static_cast<void>(f.call(cf, "_CFBitVectorSetBitAtIndex", {mutable_bits, 1, 1}));
    const auto out = f.block(4, 0xff);
    static_cast<void>(f.call(cf, "_CFBitVectorGetBits", {mutable_bits, 0, 20, out}));
    require(f.memory().read8(out) == 0x60 && f.memory().read8(out + 1) == 0x01 && f.memory().read8(out + 2) == 0xf0,
            "SetCount, SetBits, Flip and GetBits");
    static_cast<void>(f.call(cf, "_CFBitVectorFlipBits", {mutable_bits, 0, 4}));
    static_cast<void>(f.call(cf, "_CFBitVectorGetBits", {mutable_bits, 0, 8, out}));
    require(f.memory().read8(out) == 0x90, "CFBitVectorFlipBits");
    static_cast<void>(f.call(cf, "_CFBitVectorSetAllBits", {mutable_bits, 0}));
    require(f.call(cf, "_CFBitVectorGetCountOfBit", {mutable_bits, 0, 20, 1}) == 0, "CFBitVectorSetAllBits");
    bool refused = false;
    try { static_cast<void>(f.call(cf, "_CFBitVectorSetBitAtIndex", {bits, 0, 0})); } catch (const std::exception&) { refused = true; }
    require(refused, "an immutable bit vector refuses changes");
}

void trees(Fixture& f) {
    const auto context = f.block(40);
    const auto make = [&](std::string_view name) {
        f.engine.write64(context + 8, string(f, name));
        f.engine.write64(context + 16, f.symbol(cf, "_CFRetain"));
        f.engine.write64(context + 24, f.symbol(cf, "_CFRelease"));
        return f.call(cf, "_CFTreeCreate", {0, context});
    };
    const auto root = make("root"), b = make("b"), a = make("a"), c = make("c");
    require(f.call(cf, "_CFGetTypeID", {root}) == f.call(cf, "_CFTreeGetTypeID"), "CFTree type id");
    static_cast<void>(f.call(cf, "_CFTreeAppendChild", {root, b}));
    static_cast<void>(f.call(cf, "_CFTreePrependChild", {root, c}));
    static_cast<void>(f.call(cf, "_CFTreeInsertSibling", {c, a}));   // c, a, b
    require(f.call(cf, "_CFTreeGetChildCount", {root}) == 3 && f.call(cf, "_CFTreeGetFirstChild", {root}) == c &&
                f.call(cf, "_CFTreeGetNextSibling", {c}) == a && f.call(cf, "_CFTreeGetNextSibling", {b}) == 0 &&
                f.call(cf, "_CFTreeGetChildAtIndex", {root, 2}) == b, "append, prepend and insert sibling");
    require(f.call(cf, "_CFTreeGetParent", {a}) == root && f.call(cf, "_CFTreeFindRoot", {a}) == root, "parent and root");
    static_cast<void>(f.call(cf, "_CFTreeSortChildren", {root, f.symbol(cf, "_CFStringCompare"), 0}));
    const auto children = f.block(24);
    static_cast<void>(f.call(cf, "_CFTreeGetChildren", {root, children}));
    require(f.engine.read64(children) == a && f.engine.read64(children + 8) == b && f.engine.read64(children + 16) == c,
            "CFTreeSortChildren by the children's infos");
    const auto read = f.block(40);
    static_cast<void>(f.call(cf, "_CFTreeGetContext", {b, read}));
    require(f.cf.utf8(f.engine.read64(read + 8)) == "b" && f.engine.read64(read + 16) == f.symbol(cf, "_CFRetain"), "CFTreeGetContext");
    const auto count = f.call(cf, "_CFGetRetainCount", {b});
    static_cast<void>(f.call(cf, "_CFTreeApplyFunctionToChildren", {root, f.symbol(cf, "_CFRetain"), 0}));
    require(f.call(cf, "_CFGetRetainCount", {b}) == count + 1, "CFTreeApplyFunctionToChildren");
    static_cast<void>(f.call(cf, "_CFRelease", {b}));
    static_cast<void>(f.call(cf, "_CFRetain", {b}));
    static_cast<void>(f.call(cf, "_CFTreeRemove", {b}));
    require(f.call(cf, "_CFTreeGetChildCount", {root}) == 2 && f.call(cf, "_CFTreeGetParent", {b}) == 0, "CFTreeRemove");
    static_cast<void>(f.call(cf, "_CFTreeRemoveAllChildren", {root}));
    require(f.call(cf, "_CFTreeGetChildCount", {root}) == 0, "CFTreeRemoveAllChildren");
}

void bags(Fixture& f) {
    const auto callbacks = f.symbol(cf, "_kCFTypeBagCallBacks");
    const auto bag = f.call(cf, "_CFBagCreate", {0, values(f, {string(f, "x"), string(f, "y"), string(f, "x")}), 3, callbacks});
    require(f.call(cf, "_CFGetTypeID", {bag}) == f.call(cf, "_CFBagGetTypeID"), "CFBag type id");
    require(f.call(cf, "_CFBagGetCount", {bag}) == 3 && f.call(cf, "_CFBagGetCountOfValue", {bag, string(f, "x")}) == 2 &&
                yes(f.call(cf, "_CFBagContainsValue", {bag, string(f, "y")})) && !yes(f.call(cf, "_CFBagContainsValue", {bag, string(f, "z")})),
            "counts by CFEqual");
    const auto out = f.block(8);
    require(yes(f.call(cf, "_CFBagGetValueIfPresent", {bag, string(f, "y"), out})) && f.cf.utf8(f.engine.read64(out)) == "y", "get value");
    const auto copy = f.call(cf, "_CFBagCreateMutableCopy", {0, 0, bag});
    static_cast<void>(f.call(cf, "_CFBagAddValue", {copy, string(f, "y")}));
    static_cast<void>(f.call(cf, "_CFBagRemoveValue", {copy, string(f, "x")}));
    require(f.call(cf, "_CFBagGetCountOfValue", {copy, string(f, "x")}) == 1 && f.call(cf, "_CFBagGetCountOfValue", {copy, string(f, "y")}) == 2 &&
                f.call(cf, "_CFBagGetCount", {bag}) == 3, "add and remove one occurrence of a copy");
    static_cast<void>(f.call(cf, "_CFBagReplaceValue", {copy, string(f, "z")}));
    require(!yes(f.call(cf, "_CFBagContainsValue", {copy, string(f, "z")})), "replace only a present value");
    static_cast<void>(f.call(cf, "_CFBagSetValue", {copy, string(f, "z")}));
    require(f.call(cf, "_CFBagGetCountOfValue", {copy, string(f, "z")}) == 1, "set adds an absent value");
    const auto words = f.call(cf, "_CFBagCreateMutable", {0, 0, callbacks});
    const auto one = mutable_string(f, "a"), two = mutable_string(f, "b");
    static_cast<void>(f.call(cf, "_CFBagAddValue", {words, one}));
    static_cast<void>(f.call(cf, "_CFBagAddValue", {words, two}));
    static_cast<void>(f.call(cf, "_CFBagApplyFunction", {words, f.symbol(cf, "_CFStringAppend"), string(f, "!")}));
    require(f.cf.utf8(one) == "a!" && f.cf.utf8(two) == "b!", "CFBagApplyFunction");
    static_cast<void>(f.call(cf, "_CFBagRemoveAllValues", {words}));
    require(f.call(cf, "_CFBagGetCount", {words}) == 0, "CFBagRemoveAllValues");
    bool refused = false;
    try { static_cast<void>(f.call(cf, "_CFBagAddValue", {bag, string(f, "x")})); } catch (const std::exception&) { refused = true; }
    require(refused, "an immutable bag refuses changes");
}

void sets(Fixture& f) {
    const auto callbacks = f.symbol(cf, "_kCFTypeSetCallBacks");
    const auto set = f.call(cf, "_CFSetCreate", {0, values(f, {string(f, "a"), string(f, "b")}), 2, callbacks});
    require(yes(f.call(cf, "_CFSetContainsValue", {set, string(f, "a")})) && !yes(f.call(cf, "_CFSetContainsValue", {set, string(f, "c")})) &&
                f.call(cf, "_CFSetGetCountOfValue", {set, string(f, "b")}) == 1, "membership");
    const auto out = f.block(8);
    require(yes(f.call(cf, "_CFSetGetValueIfPresent", {set, string(f, "b"), out})) && f.cf.utf8(f.engine.read64(out)) == "b" &&
                f.cf.utf8(f.call(cf, "_CFSetGetValue", {set, string(f, "a")})) == "a", "get value");
    const auto copy = f.call(cf, "_CFSetCreateMutableCopy", {0, 0, set});
    static_cast<void>(f.call(cf, "_CFSetSetValue", {copy, string(f, "c")}));
    static_cast<void>(f.call(cf, "_CFSetRemoveValue", {copy, string(f, "a")}));
    require(f.call(cf, "_CFSetGetCount", {copy}) == 2 && f.call(cf, "_CFSetGetCount", {set}) == 2 &&
                f.call(cf, "_CFSetGetCount", {f.call(cf, "_CFSetCreateCopy", {0, copy})}) == 2, "copies, set and remove");
    const auto strings = f.call(cf, "_CFSetCreateMutable", {0, 0, callbacks});
    const auto one = mutable_string(f, "x");
    static_cast<void>(f.call(cf, "_CFSetAddValue", {strings, one}));
    static_cast<void>(f.call(cf, "_CFSetApplyFunction", {strings, f.symbol(cf, "_CFStringAppend"), string(f, "?")}));
    require(f.cf.utf8(one) == "x?", "CFSetApplyFunction");
    static_cast<void>(f.call(cf, "_CFSetRemoveAllValues", {copy}));
    require(f.call(cf, "_CFSetGetCount", {copy}) == 0, "CFSetRemoveAllValues");
}

void arrays_and_dictionaries(Fixture& f) {
    const auto array_callbacks = f.symbol(cf, "_kCFTypeArrayCallBacks");
    const auto array = f.call(cf, "_CFArrayCreateMutable", {0, 0, array_callbacks});
    for (const auto text : {"pear", "fig", "apple", "fig"}) static_cast<void>(f.call(cf, "_CFArrayAppendValue", {array, string(f, text)}));
    const auto fig = string(f, "fig");
    require(f.call(cf, "_CFArrayGetCountOfValue", {array, 0, 4, fig}) == 2 && f.call(cf, "_CFArrayGetFirstIndexOfValue", {array, 0, 4, fig}) == 1 &&
                f.call(cf, "_CFArrayGetLastIndexOfValue", {array, 0, 4, fig}) == 3 &&
                f.call(cf, "_CFArrayGetFirstIndexOfValue", {array, 0, 4, string(f, "kiwi")}) == not_found, "counting and finding values");
    const auto compare = f.symbol(cf, "_CFStringCompare");
    static_cast<void>(f.call(cf, "_CFArraySortValues", {array, 0, 4, compare, 0}));
    std::vector<std::string> order;
    for (Address index = 0; index < 4; ++index) order.push_back(f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {array, index})));
    require(order == std::vector<std::string>{"apple", "fig", "fig", "pear"}, "CFArraySortValues");
    require(f.call(cf, "_CFArrayBSearchValues", {array, 0, 4, string(f, "fig"), compare, 0}) == 1 &&
                f.call(cf, "_CFArrayBSearchValues", {array, 0, 4, string(f, "banana"), compare, 0}) == 1 &&
                f.call(cf, "_CFArrayBSearchValues", {array, 0, 4, string(f, "zucchini"), compare, 0}) == 4, "CFArrayBSearchValues");
    const auto dictionary = f.call(cf, "_CFDictionaryCreateMutable",
        {0, 0, f.symbol(cf, "_kCFTypeDictionaryKeyCallBacks"), f.symbol(cf, "_kCFTypeDictionaryValueCallBacks")});
    static_cast<void>(f.call(cf, "_CFDictionarySetValue", {dictionary, string(f, "k1"), fig}));
    static_cast<void>(f.call(cf, "_CFDictionarySetValue", {dictionary, string(f, "k2"), string(f, "fig")}));
    require(f.call(cf, "_CFDictionaryGetCountOfKey", {dictionary, string(f, "k1")}) == 1 &&
                f.call(cf, "_CFDictionaryGetCountOfKey", {dictionary, string(f, "k3")}) == 0 &&
                f.call(cf, "_CFDictionaryGetCountOfValue", {dictionary, fig}) == 2, "dictionary counts");
}

void character_sets(Fixture& f) {
    const auto set = f.call(cf, "_CFCharacterSetCreateWithCharactersInRange", {0, 'a', 26});
    require(f.call(cf, "_CFGetTypeID", {set}) == f.call(cf, "_CFCharacterSetGetTypeID"), "CFCharacterSet type id");
    require(yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {set, 'q'})) && !yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {set, 'Q'})),
            "CFCharacterSetCreateWithCharactersInRange");
    const auto vowels = f.call(cf, "_CFCharacterSetCreateWithCharactersInString", {0, string(f, "aeiou")});
    require(yes(f.call(cf, "_CFCharacterSetIsSupersetOfSet", {set, vowels})) && !yes(f.call(cf, "_CFCharacterSetIsSupersetOfSet", {vowels, set})),
            "CFCharacterSetIsSupersetOfSet");
    const auto inverted = f.call(cf, "_CFCharacterSetCreateInvertedSet", {0, vowels});
    require(!yes(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {inverted, 'e'})) &&
                yes(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {inverted, 0x1f600})) &&
                yes(f.call(cf, "_CFCharacterSetHasMemberInPlane", {inverted, 1})) && !yes(f.call(cf, "_CFCharacterSetHasMemberInPlane", {vowels, 1})),
            "inverted set and planes");
    const auto consonants = f.call(cf, "_CFCharacterSetCreateMutableCopy", {0, set});
    static_cast<void>(f.call(cf, "_CFCharacterSetRemoveCharactersInString", {consonants, string(f, "aeiou")}));
    static_cast<void>(f.call(cf, "_CFCharacterSetRemoveCharactersInRange", {consonants, 'x', 3}));
    require(!yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'e'})) && !yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'y'})) &&
                yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'b'})), "removing characters");
    static_cast<void>(f.call(cf, "_CFCharacterSetIntersect", {consonants, f.call(cf, "_CFCharacterSetCreateWithCharactersInString", {0, string(f, "abc")})}));
    require(yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'b'})) && !yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'd'})),
            "CFCharacterSetIntersect");
    static_cast<void>(f.call(cf, "_CFCharacterSetInvert", {consonants}));
    require(yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'a'})) && !yes(f.call(cf, "_CFCharacterSetIsCharacterMember", {consonants, 'b'})),
            "CFCharacterSetInvert");
    const auto bitmap = f.call(cf, "_CFCharacterSetCreateBitmapRepresentation", {0, inverted});
    const auto round_trip = f.call(cf, "_CFCharacterSetCreateWithBitmapRepresentation", {0, bitmap});
    require(!yes(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {round_trip, 'e'})) &&
                yes(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {round_trip, 0x1f600})) &&
                yes(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {round_trip, 'z'})), "bitmap round trip");
    require(yes(f.send(f.send(f.cls("NSCharacterSet"), "characterSetWithBitmapRepresentation:", {bitmap}), "longCharacterIsMember:", {'z'})),
            "+[NSCharacterSet characterSetWithBitmapRepresentation:]");
}

} // namespace

int main() {
    return run("corefoundation collections", {{"binary heaps", binary_heaps}, {"bit vectors", bit_vectors}, {"trees", trees},
        {"bags", bags}, {"sets", sets}, {"arrays and dictionaries", arrays_and_dictionaries}, {"character sets", character_sets}});
}
