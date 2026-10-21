#pragma once

// G1 data providers (docs/ALCHEMY_QLAB_GAPS.md, group G1), one install
// function per file of this directory; install.cpp calls them in order.
//   guest.cpp              guest helpers (strings, collections, blocks, errors)
//   class_objects.cpp      Objective-C class objects in CF collections
//   sorting.cpp            NSSortDescriptor and the NSArray/NSMutableArray sorts
//   collections_extra.cpp  NSArray, NSMutableArray, NSDictionary, NSSet,
//                          NSMutableSet, NSIndexSet additions
//   pointer_collections.cpp NSHashTable, NSMapTable, NSPointerArray, NSIndexPath
//   strings.cpp            NSString / NSMutableString additions, NSCharacterSet URL sets
//   scanner.cpp            NSScanner
//   regex_provider.cpp     NSRegularExpression, NSTextCheckingResult (core: regex.cpp)
//   predicate_provider.cpp NSPredicate, NSComparisonPredicate, NSCompoundPredicate (core: predicate.cpp)
//   decimal_provider.cpp   NSDecimalNumber, NSDecimalNumberHandler, NSDecimal functions (core: decimal.cpp)
//   calendar_provider.cpp  NSCalendar, NSDateComponents, NSDate additions (core: calendar.cpp)
//   formatters_extra.cpp   NSByteCountFormatter, NSListFormatter, NSFormatter/NSNumberFormatter/NSLocale additions
//   data_extra.cpp         NSData base64/compression/no-copy, file reads of strings and dictionaries
//   xml_provider.cpp       NSXMLDocument, NSXMLNode, NSXMLElement (core: xml.cpp)
//   uttype_provider.cpp    UTType, UniformTypeIdentifiers constants, UTCreateStringForOSType (core: uttype.cpp)
//   url_extra.cpp          NSURL bookmarks, filePathURL, resource values
//   compression_provider.cpp /usr/lib/libcompression.dylib (core: compression.cpp)
//   archives_provider.cpp  /usr/lib/libbz2.1.0.dylib, /usr/lib/libxar.1.dylib (cores: bzip2.cpp, xar.cpp)
//   coding.cpp             keyed coding of NSDate and NSURL (nibs); NSMutableAttributedString attribute fixing
#include "alchemy/darwin/foundation/guest.hpp"

#include <algorithm>

namespace alchemy::gui::completion_kit::data {

void install_class_objects(Support& support);
void install_sorting(Support& support);
void install_collections_extra(Support& support);
void install_pointer_collections(Support& support);
void install_strings(Support& support);
void install_scanner(Support& support);
void install_regex(Support& support);
void install_predicates(Support& support);
void install_decimals(Support& support);
void install_calendars(Support& support);
void install_formatters_extra(Support& support);
void install_data_extra(Support& support);
void install_xml(Support& support);
void install_uttypes(Support& support);
void install_url_extra(Support& support);
void install_coding(Support& support);
void install_compression(Support& support);
void install_archives(Support& support);

// Shared by the providers: sorts `values` (stable) with a comparator that
// returns an NSComparisonResult.
template <typename Compare>
void stable_sort(std::vector<Address>& values, Compare compare) {
    std::stable_sort(values.begin(), values.end(), [&](Address a, Address b) { return compare(a, b) < 0; });
}
// -[NSSortDescriptor compareObject:toObject:] for an NSSortDescriptor (host).
std::int64_t compare_with_descriptors(Support& support, Address descriptors, Address left, Address right);

} // namespace alchemy::gui::completion_kit::data
