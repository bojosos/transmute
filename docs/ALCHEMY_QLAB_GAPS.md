# QLab 5.6.3 gap list and batch plan

This is the complete list of interfaces that QLab 5.6.3 references and
Alchemy does not implement yet. It is split into six implementation groups.
Each group owns disjoint files, so parallel agents can work without merge
conflicts. It complements [the QLab audit](ALCHEMY_QLAB_AUDIT.md). The audit
counts binding records, while this list is selector- and class-level.

Measured on `mac/qlab-scan`, which is `mac/qlab@84241e7` plus the diagnostics
below.

## How the list was made

**Static scan.** `scripts/alchemy_objc_gap_scan.py` reads only metadata.
QLab's machine code is not read or disassembled. For every arm64 Mach-O in the
bundle (main executable, SnoizeMIDI, Syphon, Sparkle and its helpers, libndi)
it reads:
* load commands and symbol tables, including imported `_OBJC_CLASS_$_` and
  `_OBJC_METACLASS_$_` symbols;
* chained fixups or dyld-info binds;
* `__objc_classlist`, `__objc_catlist`, `__objc_classrefs`,
  `__objc_superrefs`, `__objc_selrefs` and `__objc_protolist`.

It also reads every compiled nib (`NIBArchive` and keyed-archive plists) for
class names, coder keys, runtime attributes and bindings.

**Implementation side.** This comes from the runtime, not from grepping the
sources. `ALCHEMY_DUMP_OBJC=<file>` makes alchemy(-fx) write every registered
class with its host-implemented and guest-implemented selectors. C imports
come from `alchemy doctor --deps --json`, as unresolved strong imports.

**Dynamic probe.** With `ALCHEMY_PROBE_MISSING=zero`, an unimplemented
message logs `[alchemy probe] message -[Class sel]` once per (class,
selector) and returns 0/nil. Strict mode is unchanged. The run was cloaked,
with no input injection and no window activation, and used a fresh guest
HOME. QLab reaches its *Welcome / license agreement* window and waits there.
The run was stopped at that window, so dynamic results cover launch up to the
EULA only. **Nothing in this list was seen past the EULA.** One earlier run
(`zero3`) was started with `-EULA-accepted YES` as an argument-domain
default. It aborted during startup (a dispatch queue used as an Objective-C
object; see G2) before any message was probed. It wrote no preferences and
contributed no entries. The flag is not used by any run behind this list.

**Classification.**
* A referenced selector is a gap only when:
  * no guest class or category defines it, and
  * no host class implements it.
* Selectors declared only by delegate or data-source protocols (328) are
  callbacks that AppKit sends *to* the app. They are listed in the JSON
  (`callback_selectors`) and are not gaps.
* Owners come from protocol declarations (NSAccessibility, CAMediaTiming,
  ...) or from `scripts/alchemy_objc_attribution.json`, a selector → class
  table written from the public API. Every gap selector has an owner; class
  attribution of instance selectors is still the best public-API guess.
* `h` marks selectors referenced only by Sparkle's helper processes
  (Autoupdate, Updater.app, XPC services). Those processes are not part of the
  QLab process.

**Reproduce:**

```powershell
$env:ALCHEMY_DUMP_OBJC = "$scratch\dump.json"; $env:ALCHEMY_PROBE_MISSING = 'zero'   # plus the cloak env of the run script
alchemy-fx --runtime-dir ... --root C:\dev\transmute\artifacts\alchemy-root /Applications/QLab.app 2> probe.txt
python -I scripts/alchemy_objc_gap_scan.py <root>\Applications\QLab.app --out $scratch\out --dump $scratch\dump.json `
  --alchemy alchemy-fx.exe --runtime-dir ... --root <root> --sources src/alchemy --probe probe.txt
```

The script writes `scan.json` (facts per image) and `gaps.json` (this list,
with a group on every entry).

## Totals

| Category | Count | Note |
| --- | --- | --- |
| (a) System classes referenced, not registered | 110 | 14 subclassed by the app; nib-internal classes that the nib loader decodes by name are excluded |
| (b) Selectors with no host or guest implementation | 711 | A further 328 are delegate callbacks (not gaps) |
| (c) Unresolved strong C imports (doctor, QLab + bundled frameworks) | 372 | AudioToolbox 62, OpenGL 40 (Syphon), libSystem 37, CoreMIDI 35, CryptoKit 24, CoreMedia 22, ... |
| (d) Nib classes not decodable | 46 | 36 class missing, 7 without `-initWithCoder:`, 3 rejected by the loader (Auto Layout constraints, Cocoa bindings, some NSColor color spaces) |
| Dynamic (pre-EULA probe run) | 6 | In first-seen order below |

Per group:

| Group | Classes | Selectors | C symbols | Nib entries | Dynamic | Size |
| --- | --- | --- | --- | --- | --- | --- |
| G1 Foundation data & serialization | 13 | 200 | 15 | 3 | 2 | L (~3.5k lines) |
| G2 Foundation runtime, process & system | 26 | 124 | 73 | 3 | 1 | L (~3.5k lines) |
| G3 AppKit controls, text, tables, accessibility | 21 | 117 | 0 | 17 | 1 | L (~3k lines) |
| G4 AppKit windows, documents, Auto Layout, nibs | 14 | 110 | 6 | 13 | 1 | XL (~4k lines; constraint solver) |
| G5 Media, audio & MIDI | 12 | 60 | 166 | 0 | 0 | L (~3k lines) |
| G6 Graphics: QuartzCore, CoreImage, GL, Swift overlays | 24 | 100 | 112 | 10 | 1 | L (~3k lines) |

### Dynamic gaps (first-seen order)

The pre-EULA run shows six, and none of them is fatal under the probe:

1. `+[CAMediaTimingFunction functionWithName:]` (G6)
2. `-[NSImageView animator]` (G4: the NSAnimatablePropertyContainer proxy is NSView's; listed under G3 by receiver class)
3. `-[NSTextField animator]` (G4, same)
4. `-[NSHTTPCookieStorage cookiesForURL:]` (G2)
5. `+[NSKeyedArchiver archivedDataWithRootObject:requiringSecureCoding:error:]` (G1)
6. `-[NSMutableArray indexesOfObjectsPassingTest:]` (G1)

A run in the default trap mode (`ALCHEMY_PROBE_MISSING=trap`, message probe
off) with a fresh guest HOME stops first at `+[NSData
dataWithContentsOfURL:]` (G1). The zero-mode runs did not reach it, because
an earlier zero return changes the path. Treat it as dynamic #0 for G1.

One more fatal object-model gap, tagged **with-flag run (zero3)**: a
`dispatch_queue_t` reached `objc_msgSend` and failed with "invalid ...
compatibility object (isa ... not a registered class)". It is in G2.
`OS_dispatch_queue` is also a static class gap (Swift Dispatch overlay).

**Next dynamic run.** Repeat the probe after the license window is passed
legitimately, which needs the user's decision. That run will list the
main-window gaps in first-seen order. The main-window nibs (`Workspace.nib`,
`CueListPane.nib`, `Inspector.nib`, `StatusWindow.nib`) already name these
static gaps: `NSLayoutConstraint`, `NSSegmentedControl`, `NSPopover`,
`NSShadow`, `NSPathControl`, `NSSortDescriptor`, `NSSearchFieldCell`,
`NSTextTab`, `NSPanGestureRecognizer`, `NSCursor`, and the text system
classes (`NSTextViewSharedData`, `NSLayoutManager`, `NSTextContainer`).

## Rules for every group

* **Own your directory.** Each group owns `src/alchemy/kit/completion/<dir>/`
  and `cmake/gui/qlab_<dir>.cmake`. Both already exist: the empty
  `install_group_<dir>` is wired into `install_completion_kit`, and the cmake
  fragment is already in `AlchemyGui.cmake`. Add files and `install_*`
  functions only there. Register tests in your own fragment with
  `alchemy_gui_add_unit_test`.
* **Exclusively owned existing files.** The files listed under a group may
  be edited only by that group. Every other existing file is read-only for
  all groups. To add methods to a class that another file registers, use
  `objc.find_class` plus `add_native_method` from your own directory, as
  `kit/completion/` already does. Don't add methods to the shared
  `kit/completion/constants.cpp`. Put new string constants and exports in your
  own directory.
* **Fail explicitly.** An unsupported path calls `completion_kit::unsupported(...)` in
  strict mode, as `docs/ALCHEMY_PROVIDER_PATTERN` and the existing providers
  do. No empty stubs, no download of Apple code, no guest patching.
* **Tests.** Host-only cores get unit tests in `<dir>/tests/` (pattern:
  `kit/completion/tests/qlab_support_tests.cpp`). Keep
  `ctest -R "^alchemy_"` green.
* **Verify.** After implementing, re-run the probe (`ALCHEMY_PROBE_MISSING=zero`)
  and the static scan. The entries of your group must disappear from
  `gaps.json`.

---

## G1: Foundation data and serialization

**Scope.** NSKeyedArchiver and NSKeyedUnarchiver as real keyed archives. They
are binary plists, written with the `NSCoder` `encode…:forKey:` set and
secure-coding class lists. QLab archives cues and the pasteboard and
preferences through them, and NSKeyedArchiver is dynamic gap #5.

The group also covers:
* NSData file, URL, base64 and compression I/O, with `libcompression`
  semantics (`COMPRESSION_ZLIB`/`LZFSE`: implement only the algorithms that are
  used, and reject the others explicitly);
* NSString and NSMutableString additions;
* the collection additions: NSArray, NSMutableArray, NSDictionary, NSSet,
  NSIndexSet and the block/predicate variants, plus NSHashTable, NSMapTable,
  NSPointerArray and NSIndexPath;
* NSScanner, NSRegularExpression (ICU-free: write a small backtracking
  engine and reject unsupported syntax explicitly), NSPredicate (format parser
  for the forms that are used) and NSSortDescriptor;
* NSDecimalNumber and NSDecimalNumberHandler;
* NSCalendar and NSDateComponents (Gregorian), NSDate additions,
  NSByteCountFormatter and NSListFormatter;
* NSXMLDocument and NSXMLNode (XPath subset; used by Sparkle appcasts only);
* UTType, the UTType constants and `UTCreateStringForOSType`.

**Owns.** `src/alchemy/kit/completion/data/`, `cmake/gui/qlab_data.cmake`, and
exclusive edits to:
* `src/alchemy/foundation_extra.cpp` and `src/alchemy/foundation_bridge.cpp`
  (NSArray, NSDictionary, NSString core);
* `src/alchemy/fwgaps/foundation.cpp` and `src/alchemy/fwgaps/charset.cpp`;
* `src/alchemy/kit/completion/{json,json_serialization,formatters,number_format,date_format,collections,immutable_copies}.*`.

**Tests.** Unit tests for:
* an archiver round trip, written by the host and read by the host. Also
  decode a keyed archive fixture produced by the project's own writer, never
  an Apple binary;
* the scanner, regex and predicate parsers;
* decimal arithmetic and rounding;
* calendar components and date arithmetic;
* byte-count strings.

### G1 item list

Dynamic (first-seen order of the pre-EULA probe run):

5. `+[NSKeyedArchiver archivedDataWithRootObject:requiringSecureCoding:error:]`
6. `-[NSMutableArray indexesOfObjectsPassingTest:]`

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
NSByteCountFormatter, NSDecimalNumber, NSDecimalNumberHandler, NSHashTable, NSIndexPath, NSListFormatter, NSMapTable, NSPointerArray, NSPredicate, NSRegularExpression, NSSortDescriptor, NSXMLDocument, UTType

Nib decoding (class: status, nib count):
NSDate: no-initWithCoder (1), NSSortDescriptor: class-missing (3), NSURL: no-initWithCoder (4)

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **NSKeyedArchiver**: `archivedDataWithRootObject:requiringSecureCoding:error:` `archivedDataWithRootObject:` `encodeConditionalObject:forKey:` `finishEncoding` `initForWritingWithMutableData:` `initRequiringSecureCoding:`
- **NSArray**: `enumerateObjectsWithOptions:usingBlock:` `filteredArrayUsingPredicate:` `indexOfObjectPassingTest:` `indexesOfObjectsPassingTest:` `indexesOfObjectsWithOptions:passingTest:` `initWithArray:copyItems:` `makeObjectsPerformSelector:` `makeObjectsPerformSelector:withObject:` `objectsAtIndexes:` `sortedArrayUsingDescriptors:` `sortedArrayUsingFunction:context:` `sortedArrayUsingSelector:` `subarrayWithRange:`
- **NSArray/NSDictionary**: `writeToFile:atomically:`
- **NSArray/NSDictionary/NSData**: `writeToURL:atomically:`
- **NSByteCountFormatter**: `setZeroPadsFractionDigits:` `stringFromByteCount:` `stringFromByteCount:countStyle:`
- **NSCalendar**: `component:fromDate:` `components:fromDate:` `components:fromDate:toDate:options:` `currentCalendar` `dateByAddingUnit:value:toDate:options:` `dateFromComponents:` `initWithCalendarIdentifier:`
- **NSCharacterSet**: `URLFragmentAllowedCharacterSet` `URLQueryAllowedCharacterSet`
- **NSData**: `base64EncodedStringWithOptions:` `compressedDataUsingAlgorithm:error:` `dataWithBytesNoCopy:length:freeWhenDone:` `dataWithContentsOfFile:` `dataWithContentsOfFile:options:error:` `dataWithContentsOfURL:` `dataWithContentsOfURL:options:error:` `dataWithData:` `decompressedDataUsingAlgorithm:error:` `initWithBase64EncodedString:options:` `writeToURL:options:error:`
- **NSData (NSFileManager-style)**: `writeToURL:error:`
- **NSDate**: `dateWithTimeInterval:sinceDate:` `now`
- **NSDate/NSNumber**: `descriptionWithLocale:`
- **NSDateComponents**: `setHour:` `setMinute:` `setSecond:` `weekday`
- **NSDecimalNumber**: `decimalNumberByDividingBy:` `decimalNumberByMultiplyingBy:` `decimalNumberByMultiplyingByPowerOf10:` `decimalNumberByRoundingAccordingToBehavior:` `decimalNumberHandlerWithRoundingMode:scale:raiseOnExactness:raiseOnOverflow:raiseOnUnderflow:raiseOnDivideByZero:` `decimalNumberWithDecimal:` `decimalNumberWithMantissa:exponent:isNegative:` `decimalNumberWithString:` `decimalNumberWithString:locale:` `notANumber`
- **NSDictionary**: `allKeysForObject:` `dictionaryWithContentsOfFile:` `initWithDictionary:copyItems:` `keysOfEntriesPassingTest:`
- **NSFormatter**: `setFormattingContext:`
- **NSHashTable**: `hashTableWithOptions:` `weakObjectsHashTable`
- **NSHashTable/NSPointerArray**: `initWithOptions:capacity:`
- **NSIndexPath**: `indexPathForItem:inSection:` `section`
- **NSIndexSet**: `countOfIndexesInRange:` `enumerateIndexesWithOptions:usingBlock:` `indexesPassingTest:`
- **NSKeyedArchiver (NSCoder)**: `encodeBool:forKey:` `encodeBytes:length:forKey:` `encodeDouble:forKey:` `encodeFloat:forKey:` `encodeInt32:forKey:` `encodeInt64:forKey:` `encodeInt:forKey:` `encodeInteger:forKey:` `encodeObject:forKey:` `encodePoint:forKey:` `encodeRect:forKey:` `encodeSize:forKey:`
- **NSKeyedArchiver/NSKeyedUnarchiver**: `setRequiresSecureCoding:`
- **NSKeyedUnarchiver**: `finishDecoding` `initForReadingFromData:error:` `initForReadingWithData:` `setDecodingFailurePolicy:` `unarchiveObjectWithData:` `unarchivedObjectOfClass:fromData:error:` `unarchivedObjectOfClasses:fromData:error:`
- **NSListFormatter**: `localizedStringByJoiningStrings:`
- **NSLocale**: `localizedStringForLanguageCode:`
- **NSMapTable**: `initWithKeyOptions:valueOptions:capacity:` `strongToStrongObjectsMapTable` `strongToWeakObjectsMapTable`
- **NSMutableArray**: `filterUsingPredicate:` `insertObjects:atIndexes:` `removeObjectsAtIndexes:` `removeObjectsInRange:` `setArray:` `sortUsingFunction:context:` `sortUsingSelector:` `sortWithOptions:usingComparator:`
- **NSMutableAttributedString**: `fixAttributesInRange:` `fixFontAttributeInRange:` `fixParagraphStyleAttributeInRange:` `setAlignment:range:`
- **NSMutableDictionary**: `removeObjectsForKeys:`
- **NSMutableSet**: `intersectSet:` `minusSet:` `setSet:`
- **NSMutableString**: `appendFormat:`
- **NSNumber/NSDecimalNumber**: `decimalValue`
- **NSNumberFormatter**: `setHasThousandSeparators:` `setNegativeInfinitySymbol:` `setPositiveInfinitySymbol:` `setRoundingIncrement:` `setThousandSeparator:`
- **NSPointerArray**: `compact` `pointerAtIndex:` `replacePointerAtIndex:withPointer:`
- **NSPredicate**: `evaluateWithObject:` `predicateFormat` `predicateWithBlock:` `predicateWithFormat:` `predicateWithValue:`
- **NSRegularExpression**: `escapedPatternForString:` `firstMatchInString:options:range:` `matchesInString:options:range:` `numberOfMatchesInString:options:range:` `regularExpressionWithPattern:options:error:`
- **NSScanner**: `isAtEnd` `scanCharactersFromSet:intoString:` `scanDecimal:` `scanDouble:` `scanFloat:` `scanInt:` `scanInteger:` `scanLocation` `scanString:intoString:` `scanUpToCharactersFromSet:intoString:` `scanUpToString:intoString:` `scannerWithString:` `setCharactersToBeSkipped:` `setScanLocation:`
- **NSSet**: `intersectsSet:` `isSubsetOfSet:` `objectsPassingTest:` `setByAddingObject:` `setByAddingObjectsFromArray:` `setByAddingObjectsFromSet:`
- **NSSortDescriptor**: `initWithKey:ascending:` `sortDescriptorWithKey:ascending:` `sortDescriptorWithKey:ascending:selector:`
- **NSString**: `capitalizedString` `capitalizedStringWithLocale:` `compare:options:` `componentsSeparatedByCharactersInSet:` `enumerateSubstringsInRange:options:usingBlock:` `initWithBytesNoCopy:length:encoding:freeWhenDone:` `localizedCaseInsensitiveCompare:` `localizedCaseInsensitiveContainsString:` `localizedStandardCompare:` `localizedStandardRangeOfString:` `pathWithComponents:` `rangeOfCharacterFromSet:` `rangeOfCharacterFromSet:options:` `rangeOfCharacterFromSet:options:range:` `rangeOfString:options:` `rangeOfString:options:range:` `stringByAddingPercentEncodingWithAllowedCharacters:` `stringByAppendingFormat:` `stringByPaddingToLength:withString:startingAtIndex:` `stringByReplacingCharactersInRange:withString:` `stringByReplacingOccurrencesOfString:withString:options:range:` `stringByResolvingSymlinksInPath` h `stringByTrimmingCharactersInSet:` `stringWithContentsOfFile:encoding:error:` `stringWithContentsOfURL:encoding:error:` `writeToFile:atomically:encoding:error:` `writeToURL:atomically:encoding:error:`
- **NSURL**: `URLByResolvingBookmarkData:options:relativeToURL:bookmarkDataIsStale:error:` `bookmarkDataWithOptions:includingResourceValuesForKeys:relativeToURL:error:` `filePathURL` `setResourceValue:forKey:error:` h
- **NSXMLDocument**: `initWithData:options:error:`
- **NSXMLElement**: `attributeForName:`
- **NSXMLNode**: `URI` `childAtIndex:` `localName` `nextSibling` `nodesForXPath:error:`
- **UTType**: `conformsToType:` `typeWithFilenameExtension:` `typeWithIdentifier:`

Unresolved strong C imports (doctor), by library:

- **UniformTypeIdentifiers** (11, Sparkle-helper): `_UTTypeAudio` `_UTTypeAudiovisualContent` `_UTTypeFolder` `_UTTypeGIF` `_UTTypeImage` `_UTTypeJPEG` `_UTTypeJSON` `_UTTypeMIDI` `_UTTypeMovie` `_UTTypePNG` `_UTTypeTIFF`
- **CoreServices** (4, Sparkle-helper): `_LSCopyApplicationURLsForURL` `_UTCreateStringForOSType` `_kUTTypeApplication` `_kUTTypePNG`

Totals: 2 dynamic, 13 classes, 200 selectors, 15 C symbols, 3 nib entries.


---

## G2: Foundation runtime, process and system

**Scope.**
* **Dispatch objects as Objective-C objects.** `OS_dispatch_queue` and
  `OS_dispatch_*` classes with retain/release/description: a
  `dispatch_queue_t` reaches `objc_msgSend` in QLab's startup (the "not a
  registered class" abort). Also `dispatch_queue_set_specific`,
  `dispatch_queue_get_specific`, `dispatch_get_specific` and
  `dispatch_io_*`.
* **NSException.** The class, `raise`, `+raise:format:`, `reason`, call
  stacks, and the `NSGenericException`/`NSRangeException`/`NSInternalInconsistencyException`
  and `OBJC_EHTYPE_$_NSException` exports. Objective-C exceptions stay
  unsupported for unwinding, so `raise` fails explicitly with the reason
  text.
* **Concurrency primitives.** NSLock (`tryLock`), NSInvocation and
  NSMethodSignature (`instanceMethodSignatureForSelector:`), NSOperation,
  NSBlockOperation and `-[NSOperationQueue addOperation:]`,
  NSNotificationQueue, and NSThread `sleepForTimeInterval:` and
  `callStackSymbols`.
* **NSUndoManager.** Subclassed by QLab:
  `prepareWithInvocationTarget:` (needs NSInvocation), action names, and
  undo/redo state.
* **Cocoa bindings.** `bind:toObject:withKeyPath:options:`, NSArrayController,
  NSObjectController and `_NSManagedProxy` for nibs, and KVO
  `will/didChangeValueForKey:` with the indexed variants and
  `mutableArrayValueForKey:`. NSUserDefaultsController already exists.
* **Processes and files.** NSTask, NSPipe, NSFileHandle (with the readability
  handler and background notifications) and NSInputStream, plus the
  NSFileManager additions: enumerators, URL directory queries, replace, links
  and contents comparisons. Also NSBundle (`bundleWithURL:`, `classNamed:`,
  ...) and the NSProcessInfo automatic-termination calls.
* **Scripting.** NSAppleScript and the scripting classes: NSScriptCommand,
  NSScriptClassDescription, specifiers, and the subclassed `NSDeleteCommand`
  and `NSMoveCommand`. AppleScript execution itself fails explicitly.
* **System services.** NSXPCConnection, NSXPCListener and NSXPCInterface
  (Sparkle; fail explicitly or keep it in-process), NSHTTPCookieStorage
  (dynamic gap #4), OSLogStore and OSLogEntry, `asl_*`, `OSAtomic*`,
  `OSSpinLock*`, termios `cfmakeraw`/`cfsetspeed`, `mach_wait_until`,
  `memset_pattern4`, `reallocf`, `arc4random_uniform`, `CFMessagePort*`
  (Syphon), `CFAllocator*`, `CFStringCreateWithFormat`, IOKit power
  assertions, `SMJobSubmit`/`SMJobRemove` (explicit failure), and
  `objc_copyStruct`/`objc_terminate`.

**Owns.** `src/alchemy/kit/completion/runtime/`, `cmake/gui/qlab_runtime.cmake`,
and exclusive edits to:
* `src/alchemy/darwin_dispatch.cpp`, `src/alchemy/darwin_libsystem_extra.cpp`
  and `src/alchemy/core_foundation.cpp`;
* `src/alchemy/objc_runtime.cpp` (exceptions and `objc_copyStruct` only);
* `src/alchemy/network/operation_queue.cpp` and `src/alchemy/runloop/*`;
* `src/alchemy/devices/iokit.cpp`;
* `src/alchemy/kit/completion/{foundation,kvc,oslog,file_attributes,disk_arbitration}.cpp`;
* `src/alchemy/appkit/controllers.cpp`, for the bindings controllers.

G4 calls G2's `bind:` from `nib.cpp` for `NSNibBindingConnector`. Agree on the
selector contract only; no file is shared.

**Tests.** Unit tests for:
* dispatch-object messaging (retain/release through `objc_msgSend` on a
  queue);
* NSInvocation argument marshalling;
* the undo stack;
* KVO notification order;
* NSTask/NSPipe over the guest process model (an existing test fixture);
* NSFileHandle reads.

### G2 item list

Dynamic (first-seen order of the pre-EULA probe run):

4. `-[NSHTTPCookieStorage cookiesForURL:]`

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
*NSDeleteCommand, *NSMoveCommand, *NSOperation (helper), *NSUndoManager, NSAppleScript, NSArrayController (nib), NSBlockOperation, NSException, NSFileHandle, NSInputStream (helper), NSInvocation, NSLock, NSNotificationQueue, NSPipe, NSPropertySpecifier, NSScriptClassDescription, NSScriptCommand, NSTask, NSUniqueIDSpecifier, NSXPCConnection, NSXPCInterface, NSXPCListener (helper), OSLogEntryLog, OSLogStore, OS_dispatch_queue, _NSManagedProxy (nib)

Nib decoding (class: status, nib count):
NSArrayController: class-missing (10), NSNibBindingConnector: rejected-by-nib-loader (28), _NSManagedProxy: class-missing (10)

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **NSHTTPCookieStorage**: `cookiesForURL:` `deleteCookie:`
- **NSAppleEventDescriptor**: `paramDescriptorForKeyword:`
- **NSAppleScript**: `compileAndReturnError:` `executeAndReturnError:` `initWithSource:` `richTextSource`
- **NSBlockOperation**: `blockOperationWithBlock:`
- **NSBundle**: `URLForAuxiliaryExecutable:` `bundleWithIdentifier:` `bundleWithURL:` `classNamed:` `pathForImageResource:`
- **NSBundle (AppKit)**: `imageForResource:`
- **NSDirectoryEnumerator**: `skipDescendants` `skipDescendents` h
- **NSException**: `exceptionWithName:reason:userInfo:` `raise` `raise:format:` `reason`
- **NSFileHandle**: `availableData` `closeAndReturnError:` h `fileHandleForReadingAtPath:` h `fileHandleForUpdatingURL:error:` `fileHandleWithNullDevice` `fileHandleWithStandardError` h `fileHandleWithStandardOutput` h `readDataOfLength:` h `readDataToEndOfFile` `readDataUpToLength:error:` h `readToEndOfFileInBackgroundAndNotify` `seekToOffset:error:` `setReadabilityHandler:` `synchronizeAndReturnError:` `waitForDataInBackgroundAndNotify` `writeData:` h `writeData:error:` h
- **NSFileManager**: `URLForDirectory:inDomain:appropriateForURL:create:error:` `contentsAtPath:` `contentsEqualAtPath:andPath:` `contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:` `createFileAtPath:contents:attributes:` `createSymbolicLinkAtPath:withDestinationPath:error:` h `enumeratorAtPath:` `enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:` `getRelationship:ofDirectory:inDomain:toItemAtURL:error:` `replaceItemAtURL:withItemAtURL:backupItemName:options:resultingItemURL:error:` `subpathsOfDirectoryAtPath:error:`
- **NSInputStream**: `inputStreamWithFileAtPath:` h
- **NSInvocation**: `invocationWithMethodSignature:` `setArgument:atIndex:` `setSelector:`
- **NSLock**: `tryLock`
- **NSNotificationQueue**: `defaultQueue` `enqueueNotification:postingStyle:` `enqueueNotification:postingStyle:coalesceMask:forModes:`
- **NSObject (KVC)**: `mutableArrayValueForKey:`
- **NSObject (KVO)**: `didChange:valuesAtIndexes:forKey:` `didChangeValueForKey:` `willChange:valuesAtIndexes:forKey:` `willChangeValueForKey:`
- **NSObject (NSMethodSignature)**: `instanceMethodSignatureForSelector:`
- **NSObject (bindings)**: `bind:toObject:withKeyPath:options:`
- **NSOperation**: `isCancelled` `isExecuting`
- **NSOperationQueue**: `addOperation:`
- **NSPipe**: `fileHandleForReading` `fileHandleForWriting` h `pipe`
- **NSProcessInfo**: `disableAutomaticTermination:` `enableAutomaticTermination:`
- **NSPropertySpecifier**: `initWithContainerClassDescription:containerSpecifier:key:`
- **NSScriptClassDescription**: `classDescriptionForClass:` `keyClassDescription`
- **NSScriptCommand**: `commandDescription` `currentCommand` `evaluatedArguments` `evaluatedReceivers` `setScriptErrorNumber:` `setScriptErrorString:`
- **NSScriptCommandDescription**: `commandName`
- **NSScriptObjectSpecifier**: `keySpecifier` `objectsByEvaluatingSpecifier` `objectsByEvaluatingWithContainers:`
- **NSTask**: `launch` `launchAndReturnError:` `launchedTaskWithLaunchPath:arguments:` h `setCurrentDirectoryPath:` `setEnvironment:` h `setExecutableURL:` h `setLaunchPath:` `setStandardError:` `setStandardInput:` `setStandardOutput:` `setTerminationHandler:` h `standardError` `standardOutput` `terminationStatus` `waitUntilExit`
- **NSThread**: `sleepForTimeInterval:`
- **NSThread/NSException**: `callStackReturnAddresses` `callStackSymbols`
- **NSThread/NSInvocationOperation**: `initWithTarget:selector:object:`
- **NSUndoManager**: `isRedoing` `isUndoing` `prepareWithInvocationTarget:` `removeAllActionsWithTarget:` `setActionName:` `setGroupsByEvent:`
- **NSUniqueIDSpecifier**: `initWithContainerClassDescription:containerSpecifier:key:uniqueID:`
- **NSXPCConnection**: `initWithMachServiceName:options:` `initWithServiceName:` `remoteObjectProxy` `setExportedInterface:` `setExportedObject:` `setInterruptionHandler:` `setRemoteObjectInterface:`
- **NSXPCInterface**: `interfaceWithProtocol:`
- **NSXPCListener**: `initWithMachServiceName:` h `serviceListener` h
- **OSLogEntry**: `composedMessage`
- **OSLogEntryFromProcess**: `process`
- **OSLogStore**: `localStoreAndReturnError:` `positionWithDate:`

Unresolved strong C imports (doctor), by library:

- **libSystem.B.dylib** (37, Sparkle-helper,Syphon): `_OSAtomicDecrement32` `_OSAtomicDecrement32Barrier` `_OSAtomicDequeue` `_OSAtomicEnqueue` `_OSAtomicIncrement32` `_OSAtomicIncrement32Barrier` `_OSAtomicTestAndClearBarrier` `_OSAtomicTestAndSetBarrier` `_OSSpinLockLock` `_OSSpinLockUnlock` `__os_log_create` `__os_log_internal` `_arc4random_uniform` `_asl_free` `_asl_get` `_asl_new` `_asl_next` `_asl_open` `_asl_release` `_asl_search` `_asl_send` `_asl_set` `_asl_set_query` `_cfmakeraw` `_cfsetspeed` `_dispatch_get_specific` `_dispatch_io_close` `_dispatch_io_create` `_dispatch_io_get_descriptor` `_dispatch_io_read` `_dispatch_io_write` `_dispatch_queue_get_specific` `_dispatch_queue_set_specific` `_mach_wait_until` `_matrix_identity_float4x4` `_memset_pattern4` `_reallocf`
- **CoreFoundation** (15, SnoizeMIDI,Sparkle-helper,Syphon): `_CFAllocatorAllocate` `_CFAllocatorDeallocate` `_CFArrayExchangeValuesAtIndices` `_CFBundleCopyInfoDictionaryInDirectory` `_CFMessagePortCreateLocal` `_CFMessagePortCreateRemote` `_CFMessagePortInvalidate` `_CFMessagePortIsValid` `_CFMessagePortSendRequest` `_CFMessagePortSetDispatchQueue` `_CFStringCreateWithFormat` `_NSGenericException` `_NSInternalInconsistencyException` `_NSRangeException` `_OBJC_EHTYPE_$_NSException`
- **Foundation** (12, Sparkle-helper): `_NSAllMapTableKeys` `_NSAllMapTableValues` `_NSAppleScriptErrorMessage` `_NSAppleScriptErrorNumber` `_NSAppleScriptErrorRange` `_NSEdgeInsetsZero` `_NSHomeDirectoryForUser` `_NSPointFromString` `_NSProgressFileOperationKindCopying` `_NSProgressKindFile` `_NSRectFromString` `_NSSizeFromString`
- **ServiceManagement** (4, Sparkle-helper): `_SMJobRemove` `_SMJobSubmit` `_kSMDomainSystemLaunchd` `_kSMDomainUserLaunchd`
- **IOKit** (3, Sparkle-helper): `_IOObjectIsEqualTo` `_IOPMAssertionCreateWithDescription` `_IOPMCopyAssertionsByProcess`
- **libobjc.A.dylib** (2, Sparkle-helper): `_objc_copyStruct` `_objc_terminate`

Totals: 1 dynamic, 26 classes, 124 selectors, 73 C symbols, 3 nib entries.


---

## G3: AppKit controls, text, tables and accessibility

**Scope.**
* **Missing controls**, with nib decoding (`initWithCoder:` reading the nib
  keys listed) and drawing:
  * NSSegmentedControl, NSSegmentedCell and NSSegmentItem;
  * NSSearchField and NSSearchFieldCell;
  * NSPathControl, NSPathCell and NSPathComponentCell;
  * NSDatePicker and NSDatePickerCell;
  * NSColorWell (subclassed);
  * NSComboTableView (the combo-box popup).
* **Text system nib objects.** NSTextViewSharedData, NSLayoutManager and
  NSTextContainer decoding, NSTextTab, the NSTextView attribute, selection and
  typing APIs, NSLayoutManager temporary attributes, the
  NSMutableAttributedString `fix…` methods, and NSFontManager and NSFontPanel.
* **Tables and outlines.** Row moves, drop targets, `editedColumn`, the
  row-view enumeration, and the outline insert/remove/move-with-animation
  calls.
* **NSRulerView** (subclassed) and the NSScrollView ruler and inset APIs.
* **NSCollectionView** (subclassed) with NSCollectionViewItem,
  NSCollectionViewFlowLayout, NSCollectionViewLayout and
  `_NSCollectionViewCore`: one-section flow layout, item registration and
  selection (template chooser).
* **NSAccessibility.** The accessibility property setters and getters on
  NSView, NSAccessibilityElement (subclassed) and the table accessibility
  getters. Store them; there is no screen-reader bridge.
* **Small additions.** `NSPanGestureRecognizer` nib decoding (62 nibs; a
  stored recognizer that never fires is acceptable only if it is documented),
  `NSCursor` nib decoding, NSButtonCell `highlightsBy`/`showsStateBy`,
  NSProgress file-operation keys, NSSliderCell `barRectFlipped:`, NSScroller
  `rectForPart:`, and NSTouch/NSEvent `touchesMatchingPhase:inView:`.

**Owns.** `src/alchemy/kit/completion/controls/`, `cmake/gui/qlab_controls.cmake`,
and exclusive edits to:
* `src/alchemy/appkit/{controls,controls_more,controls_internal.hpp,containers,text,rtf}.*`;
* `src/alchemy/coretext/*`.

To decode a control from a nib, implement its `-initWithCoder:` with the nib
coder methods (`decodeObjectForKey:`, ...) that `nib.cpp` already exposes. Do
not edit `nib.cpp`.

**Tests.** Unit tests for:
* the segmented-control layout and hit testing;
* the path-control component split;
* date-picker element formatting;
* collection-view flow-layout geometry;
* the text-tab and paragraph-style round trip.

Also add a nib fixture test that decodes each new control from a
project-authored NIBArchive (`nib_archive.cpp` can encode one).

### G3 item list

Dynamic (first-seen order of the pre-EULA probe run):

2. `-[NSImageView animator]`
3. `-[NSTextField animator]`

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
*NSAccessibilityElement, *NSCollectionView, *NSCollectionViewFlowLayout, *NSCollectionViewItem, *NSColorWell, *NSPathControl, *NSRulerView, *NSSearchField, NSCollectionViewLayout (nib), NSComboTableView (nib), NSDatePicker (nib), NSDatePickerCell (nib), NSPathCell (nib), NSPathComponentCell (nib), NSSearchFieldCell (nib), NSSegmentItem (nib), NSSegmentedCell (nib), NSSegmentedControl (nib), NSTextTab, NSTextViewSharedData (nib), _NSCollectionViewCore (nib)

Nib decoding (class: status, nib count):
NSComboTableView: class-missing (1), NSDatePicker: class-missing (1), NSDatePickerCell: class-missing (1), NSLayoutManager: no-initWithCoder (11), NSPanGestureRecognizer: no-initWithCoder (62), NSPathCell: class-missing (4), NSPathComponentCell: class-missing (4), NSPathControl: class-missing (4), NSSearchField: class-missing (3), NSSearchFieldCell: class-missing (4), NSSegmentItem: class-missing (3), NSSegmentedCell: class-missing (3), NSSegmentedControl: class-missing (3), NSTextContainer: no-initWithCoder (11), NSTextTab: class-missing (3), NSTextViewSharedData: class-missing (11), _NSCollectionViewCore: class-missing (1)

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **NSAccessibilityElement**: `accessibilityIdentifier`
- **NSButton**: `buttonWithImage:target:action:`
- **NSButton/NSImageView**: `setContentTintColor:`
- **NSButtonCell**: `setHighlightsBy:` `showsStateBy`
- **NSCollectionView**: `collectionViewLayout` `indexPathForItemAtPoint:` `makeItemWithIdentifier:forIndexPath:` `makeSupplementaryViewOfKind:withIdentifier:forIndexPath:` `registerClass:forItemWithIdentifier:` `registerClass:forSupplementaryViewOfKind:withIdentifier:` `selectItemsAtIndexPaths:scrollPosition:` `selectionIndexPaths` `setSelectionIndexPaths:`
- **NSCollectionViewFlowLayout**: `setItemSize:`
- **NSCollectionViewLayout**: `invalidateLayout`
- **NSDatePicker**: `dateValue` `minDate` `setDateValue:` `setDefaultDate:` `setMaxDate:` `setMinDate:`
- **NSFontManager**: `availableMembersOfFontFamily:` `fontPanel:` `selectedFont`
- **NSFontPanel**: `panelConvertFont:`
- **NSLayoutManager**: `addTemporaryAttribute:value:forCharacterRange:` `removeTemporaryAttribute:forCharacterRange:` `setAllowsNonContiguousLayout:` `textContainerChangedGeometry:`
- **NSMenu**: `insertItemWithTitle:action:keyEquivalent:atIndex:`
- **NSOutlineView**: `childIndexForItem:` `insertItemsAtIndexes:inParent:withAnimation:` `moveItemAtIndex:inParent:toIndex:inParent:` `numberOfChildrenOfItem:` `removeItemsAtIndexes:inParent:withAnimation:` `setDropItem:dropChildIndex:`
- **NSPathCell**: `pathComponentCells`
- **NSPopUpButtonCell**: `performClickWithFrame:inView:`
- **NSProgress**: `setCancellable:` `setFileCompletedCount:` `setFileOperationKind:` `setFileTotalCount:` `setKind:`
- **NSProgressIndicator (private)**: `setProgressMarks:`
- **NSRulerView**: `clientView` `registerUnitWithName:abbreviation:unitToPointsConversionFactor:stepUpCycle:stepDownCycle:` `setClientView:` `setMeasurementUnits:` `setOriginOffset:` `setReservedThicknessForAccessoryView:` `setReservedThicknessForMarkers:` `setRuleThickness:` `setRulerVisible:`
- **NSScrollView**: `addFloatingSubview:forAxis:` `contentInsets` `horizontalRulerView` `setContentInsets:` `setHasHorizontalRuler:` `setHorizontalRulerView:` `setRulersVisible:` `setScrollerInsets:`
- **NSScroller**: `rectForPart:`
- **NSSearchFieldCell**: `setSearchMenuTemplate:` `setSendsSearchStringImmediately:` `setSendsWholeSearchString:`
- **NSSegmentedControl**: `selectSegmentWithTag:` `selectedSegment` `setSelected:forSegment:` `setSelectedSegment:`
- **NSSliderCell**: `barRectFlipped:`
- **NSTableRowView**: `isEmphasized`
- **NSTableView**: `editedColumn` `enumerateAvailableRowViewsUsingBlock:` `moveRowAtIndex:toIndex:` `setDropRow:dropOperation:`
- **NSTableView (NSAccessibility)**: `accessibilityColumnHeaderUIElements` `accessibilityHeaderGroup` `accessibilityRowHeaderUIElements` `accessibilitySelectedCells` `accessibilitySelectedColumns` `accessibilityVisibleCells` `accessibilityVisibleColumns` `accessibilityVisibleRows`
- **NSTableView (section header)**: `setBottomPadding:` `setLeadingPadding:` `setTopPadding:` `setTrailingPadding:`
- **NSTextFieldCell**: `setSendsActionOnEndEditing:`
- **NSTextInputContext**: `prepareForNewContentsWithOptions:`
- **NSTextTab**: `initWithTextAlignment:location:options:`
- **NSTextView**: `selectedRanges` `selectedTextAttributes` `selectionRangeForProposedRange:granularity:` `setDefaultParagraphStyle:` `setEnabledTextCheckingTypes:` `setInlinePredictionType:` `setInsertionPointColor:` `setSelectedRanges:` `setSelectedTextAttributes:` `setTypingAttributes:` `setUsesFindPanel:`
- **NSTextView (NSResponder)**: `complete:` `insertLineBreak:`
- **NSTouch**: `normalizedPosition`
- **NSTreeNode/NSOutlineView**: `parentItem`
- **NSView (NSAccessibility)**: `setAccessibilityDescription:` `setAccessibilityFrameInParentSpace:` `setAccessibilityIdentifier:` `setAccessibilityIndex:` `setAccessibilityLabel:` `setAccessibilityParent:` `setAccessibilityRole:` `setAccessibilityRoleDescription:` `setAccessibilitySubrole:` `setAccessibilityTitle:` `setAccessibilityValue:`

Totals: 2 dynamic, 21 classes, 117 selectors, 0 C symbols, 17 nib entries.


---

## G4: AppKit windows, documents, Auto Layout and nib loading

**Scope.**
* **Animator proxy.** NSView's `animator` proxy
  (NSAnimatablePropertyContainer): apply the property at once, or over
  NSAnimationContext's duration. These are dynamic gaps #2 and #3, sent to an
  NSImageView and an NSTextField. Also `NSAnimationContext` grouping.
* **Auto Layout.** NSLayoutConstraint and NSLayoutAnchor. Decode
  constraints from nibs (47 nibs; `Workspace.nib` uses them) and solve them
  with a linear constraint solver of the Cassowary type, project-authored.
  Also `translatesAutoresizingMaskIntoConstraints`, intrinsic content size,
  hugging/compression priorities, `layoutSubtreeIfNeeded`, and
  `activate/deactivateConstraints:`.
* **Stack and grid views.** NSStackView gravity areas and
  `NSStackViewContainer`. NSGridView, NSGridRow, NSGridColumn and NSGridCell.
* **Popovers and sheets.** NSPopover (`showRelativeToRect:ofView:preferredEdge:`,
  detach and close behaviour), window sheets (`beginSheet:completionHandler:`,
  `endSheet:`), and the NSAlert additions (`alertWithError:`, the
  suppression button).
* **Panels, documents and workspace.**
  * NSOpenPanel and NSSavePanel content types.
  * NSDocument autosave, backup and draft, and
    `saveDocumentWithDelegate:...`.
  * `-[NSWindowController initWithWindowNibPath:owner:]`.
  * The NSWorkspace additions: running applications, application URLs for a
    URL, `openURLs:withApplicationAtURL:configuration:`, desktop image,
    `setIcon:`, and the accessibility display options. Add
    `NSWorkspaceOpenConfiguration`.
* **NSView additions.** Tool-tip and tracking rects, `needsToDrawRect:`,
  `replaceSubview:with:`, and concurrent drawing flags.
* **Drag and drop.** NSFilePromiseProvider (subclassed) and
  NSFilePromiseReceiver, NSDraggingImageComponent, dragging-item enumeration,
  and the NSPasteboard item APIs.
* **Nib loader.** Connector classes: `NSNibAXAttributeConnector` (55 nibs),
  `NSIBHelpConnector` (43), `NSNibAuxiliaryActionConnector` and
  `NSNibConnector`. Route `NSNibBindingConnector` (28 nibs) to G2's `bind:`.
  Remove the explicit Auto Layout rejection once constraints work. Also
  `NSTabViewItem` and the `NSWindowTemplate` keys listed.
* **Other.** NSToolbar default and principal items, the NSTabViewController
  tab items, `nextEventMatchingMask:`, and the AppKit C symbols
  (`NSDrawThreePartImage`, `NSAccessibilityPostNotificationWithUserInfo`,
  `NSViewNoIntrinsicMetric`, ...).

**Owns.** `src/alchemy/kit/completion/windows/`, `cmake/gui/qlab_windows.cmake`,
and exclusive edits to:
* `src/alchemy/appkit/{window,panels,alert,modal,documents,application,event,view,appkit,appkit_internal.hpp,nib,nib_keyed,nib_archive}.*`;
* `src/alchemy/windowserver/*`.

**Tests.** Unit tests for:
* the constraint solver (equalities, inequalities, priorities, required
  conflicts failing explicitly);
* stack and grid layout geometry;
* nib connectors from a project-authored NIBArchive;
* popover positioning.

### G4 item list

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
*NSFilePromiseProvider, NSDraggingImageComponent, NSFilePromiseReceiver, NSGridCell (nib), NSGridColumn (nib), NSGridRow (nib), NSGridView, NSIBHelpConnector (nib), NSNibAXAttributeConnector (nib), NSNibAuxiliaryActionConnector (nib), NSNibConnector (nib), NSPopover, NSStackViewContainer (nib), NSWorkspaceOpenConfiguration

Nib decoding (class: status, nib count):
NSCursor: no-initWithCoder (11), NSGridCell: class-missing (2), NSGridColumn: class-missing (2), NSGridRow: class-missing (2), NSGridView: class-missing (2), NSIBHelpConnector: class-missing (43), NSLayoutConstraint: rejected-by-nib-loader (47), NSNibAXAttributeConnector: class-missing (55), NSNibAuxiliaryActionConnector: class-missing (3), NSNibConnector: class-missing (1), NSPopover: class-missing (7), NSStackViewContainer: class-missing (1), NSTabViewItem: no-initWithCoder (1)

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **NSAlert**: `alertWithError:` `setShowsSuppressionButton:` `suppressionButton`
- **NSAnimationContext**: `groupingLevel`
- **NSApplication**: `nextEventMatchingMask:` `orderFrontCharacterPalette:` `orderFrontColorPanel:`
- **NSDocument**: `autosavedContentsFileURL` `backupFileURL` `isDraft` `saveDocumentWithDelegate:didSaveSelector:contextInfo:` `setDraft:`
- **NSDraggingImageComponent**: `draggingImageComponentWithKey:`
- **NSDraggingInfo**: `enumerateDraggingItemsWithOptions:forView:classes:searchOptions:usingBlock:` `setDraggingFormation:`
- **NSDraggingSession**: `setAnimatesToStartingPositionsOnCancelOrFail:`
- **NSEvent**: `touchesMatchingPhase:inView:`
- **NSFilePromiseReceiver**: `readableDraggedTypes` `receivePromisedFilesAtDestination:options:operationQueue:reader:`
- **NSGridView**: `cellAtColumnIndex:rowIndex:` `gridViewWithNumberOfColumns:rows:` `setColumnSpacing:` `setRowSpacing:` `viewAtColumn:`
- **NSLayoutAnchor / NSLayoutConstraint**: `bottomAnchor` `centerXAnchor` `centerYAnchor` `constraintEqualToAnchor:` `constraintEqualToAnchor:constant:` `constraintEqualToAnchor:multiplier:` `constraintEqualToConstant:` `constraintEqualToSystemSpacingAfterAnchor:multiplier:` `constraintGreaterThanOrEqualToAnchor:` `constraintGreaterThanOrEqualToAnchor:constant:` `constraintGreaterThanOrEqualToSystemSpacingAfterAnchor:multiplier:` `constraintLessThanOrEqualToAnchor:` `constraintLessThanOrEqualToAnchor:constant:` `constraintLessThanOrEqualToConstant:` `constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:` `heightAnchor` `leadingAnchor` `topAnchor` `trailingAnchor` `widthAnchor`
- **NSLayoutConstraint**: `activateConstraints:` `constant` `deactivateConstraints:` `setConstant:`
- **NSOpenPanel**: `URLsFromRunningOpenPanel`
- **NSPasteboard**: `canReadItemWithDataConformingToTypes:` `pasteboardItems`
- **NSPopover**: `showRelativeToRect:ofView:preferredEdge:`
- **NSSavePanel**: `setAllowedContentTypes:` `setAllowedTypes:` `setNameFieldLabel:`
- **NSStackView**: `addView:inGravity:` `edgeInsets` `insertArrangedSubview:atIndex:` `removeArrangedSubview:` `setDistribution:` `setEdgeInsets:` `setSpacing:` `setViews:inGravity:` `views`
- **NSTabViewController**: `setTabViewItems:`
- **NSToolbar**: `setAutosaves:` `setDefaultItemIdentifiers:` `setPrincipalItemIdentifier:`
- **NSView**: `addToolTipRect:owner:userData:` `addTrackingRect:owner:userData:assumeInside:` `layoutSubtreeIfNeeded` `mouse:inRect:` `needsToDrawRect:` `removeAllToolTips` `removeFromSuperviewWithoutNeedingDisplay` `removeToolTip:` `removeTrackingRect:` `replaceSubview:with:` `setCanDrawConcurrently:` `setClipsToBounds:` `setNeedsLayout:` `setSubviews:`
- **NSView (Auto Layout)**: `addConstraint:` `constraints` `invalidateIntrinsicContentSize` `setContentCompressionResistancePriority:forOrientation:` `setContentHuggingPriority:forOrientation:` `setNeedsUpdateConstraints:` `setTranslatesAutoresizingMaskIntoConstraints:` `translatesAutoresizingMaskIntoConstraints`
- **NSView (animator)**: `animator`
- **NSWindow**: `beginSheet:completionHandler:` `endSheet:` `endSheet:returnCode:` `setAllowsConcurrentViewDrawing:` `setBackingType:`
- **NSWindow/NSPopover**: `setContentViewController:`
- **NSWindowController**: `initWithWindowNibPath:owner:`
- **NSWorkspace**: `URLForApplicationToOpenURL:` `URLsForApplicationsToOpenURL:` `accessibilityDisplayShouldDifferentiateWithoutColor` `accessibilityDisplayShouldIncreaseContrast` `activateFileViewerSelectingURLs:` `desktopImageOptionsForScreen:` `desktopImageURLForScreen:` `isVoiceOverEnabled` `openURLs:withApplicationAtURL:configuration:completionHandler:` `runningApplications` `setDesktopImageURL:forScreen:options:error:` `setIcon:forFile:options:`

Unresolved strong C imports (doctor), by library:

- **AppKit** (6, Sparkle-helper): `_NSAccessibilityOutlineRowSubrole` `_NSAccessibilityPostNotificationWithUserInfo` `_NSCollectionElementKindSectionHeader` `_NSDrawThreePartImage` `_NSImageNameComputer` `_NSViewNoIntrinsicMetric`

Totals: 0 dynamic, 14 classes, 110 selectors, 6 C symbols, 13 nib entries.


---

## G5: Media, audio and MIDI

**Scope.**
* **CoreMIDI.** Device, entity and endpoint enumeration, virtual
  sources/destinations, properties, `MIDIReceived`, `MIDISendSysex` and
  `MIDIFlushOutput`. SnoizeMIDI creates its client at launch, so this is the
  first launch-relevant part of the group.
* **AudioToolbox.** MusicPlayer, MusicSequence, MusicTrack and
  MusicEventIterator (MIDI file cues), ExtAudioFile and AudioFile (read via
  the project's decoders), and AU parameter and event listeners.
* **AudioUnit.** Parameters and render notifications.
* **AVFoundation.**
  * Assets: AVURLAsset, AVAsset tracks and metadata, AVAssetReader and its
    track/audio-mix outputs, AVAssetImageGenerator.
  * Audio: AVAudioFormat and AVAudioChannelLayout, AVAudioUnit and
    AUAudioUnit, AVAudioUnitComponentManager.
  * Capture: AVCaptureDeviceDiscoverySession and the AVCaptureDevice format
    APIs, AVCaptureSession and AVCaptureVideoDataOutput. Capture may report
    "no devices" explicitly.
  * AVMetadataItem and the AV setting-key constants.
* **AUGenericView.**
* **CoreMedia and CoreVideo.** CMTime arithmetic, sample buffers, audio and
  video format descriptions. CVPixelBuffer, CVMetalTextureCache and host
  time.
* **VideoToolbox.** `VTDecompressionSessionDecodeFrameWithOutputHandler`.
* **Accelerate.** `vDSP_*`, `cblas_dgemm` and `dgesv_`.

Most of this is reached only by cues (audio, video, MIDI file, camera), not
by an empty workspace. Order the work by CoreMIDI, then device enumeration,
then file playback.

**Owns.** `src/alchemy/kit/completion/media/`, `cmake/gui/qlab_media.cmake`,
and exclusive edits to:
* `src/alchemy/media/*` and `src/alchemy/kit/completion/audio.cpp`;
* `src/alchemy/fwgaps/accelerate.cpp`;
* `src/alchemy/devices/{peripherals,install}.cpp`.

`devices/iokit.cpp` belongs to G2.

**Tests.** Unit tests for:
* CMTime arithmetic and comparison;
* the MusicSequence MIDI file parse (project-authored `.mid` fixture);
* MIDI endpoint enumeration with a host-side fake;
* the vDSP kernels against reference loops;
* ExtAudioFile reads of a generated WAV.

### G5 item list

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
AUGenericView, AVAssetImageGenerator, AVAssetReader, AVAssetReaderAudioMixOutput, AVAssetReaderTrackOutput, AVAudioChannelLayout, AVAudioFormat, AVAudioUnit, AVAudioUnitComponentManager, AVCaptureDeviceDiscoverySession, AVMetadataItem, AVURLAsset

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **AUAudioUnit**: `allocateRenderResourcesAndReturnError:` `deallocateRenderResources` `outputBusses` `setDeviceID:error:` `setInputEnabled:` `setInputHandler:` `setOutputEnabled:` `startHardwareAndReturnError:` `stopHardware`
- **AUAudioUnitBus**: `setFormat:error:`
- **AUCocoaUIBase (guest/AU plugin view factory)**: `uiViewForAudioUnit:withSize:`
- **AUGenericView**: `initWithAudioUnit:displayFlags:` `setShowsExpertParameters:`
- **AVAsset**: `assetWithURL:` `commonMetadata` `hasProtectedContent` `tracksWithMediaType:`
- **AVAssetImageGenerator**: `assetImageGeneratorWithAsset:` `copyCGImageAtTime:actualTime:error:`
- **AVAssetReader**: `assetReaderWithAsset:error:` `cancelReading` `setTimeRange:`
- **AVAssetReaderAudioMixOutput**: `assetReaderAudioMixOutputWithAudioTracks:audioSettings:`
- **AVAssetReaderOutput**: `copyNextSampleBuffer`
- **AVAssetReaderTrackOutput**: `assetReaderTrackOutputWithTrack:outputSettings:` `initWithTrack:outputSettings:`
- **AVAssetTrack**: `formatDescriptions` `timeRange`
- **AVAudioFormat**: `initWithCommonFormat:sampleRate:interleaved:channelLayout:`
- **AVAudioUnit**: `AUAudioUnit` `instantiateWithComponentDescription:options:completionHandler:`
- **AVAudioUnitComponent**: `audioComponentDescription`
- **AVAudioUnitComponentManager**: `componentsMatchingDescription:` `sharedAudioUnitComponentManager`
- **AVCaptureDevice**: `activeFormat` `activeVideoMinFrameDuration` `formats` `lockForConfiguration:` `setActiveFormat:` `setActiveVideoMaxFrameDuration:` `setActiveVideoMinFrameDuration:` `unlockForConfiguration`
- **AVCaptureDeviceDiscoverySession**: `discoverySessionWithDeviceTypes:mediaType:position:`
- **AVCaptureDeviceFormat**: `formatDescription` `videoSupportedFrameRateRanges`
- **AVCaptureSession**: `addInput:` `addOutput:` `beginConfiguration` `commitConfiguration` `setSessionPreset:` `startRunning` `stopRunning`
- **AVCaptureVideoDataOutput**: `setSampleBufferDelegate:queue:`
- **AVFrameRateRange**: `maxFrameDuration` `maxFrameRate` `minFrameDuration` `minFrameRate`
- **AVMetadataItem**: `commonKey`
- **AVURLAsset**: `URLAssetWithURL:options:` `initWithURL:options:`

Unresolved strong C imports (doctor), by library:

- **AudioToolbox** (62, Sparkle-helper): `_AUEventListenerAddEventType` `_AUEventListenerCreate` `_AUEventListenerRemoveEventType` `_AUListenerAddParameter` `_AUListenerCreate` `_AUListenerDispose` `_AUListenerRemoveParameter` `_AUParameterListenerNotify` `_AUParameterSet` `_AudioFileClose` `_AudioFileGetProperty` `_AudioFileGetPropertyInfo` `_AudioFileOpenURL` `_DisposeMusicEventIterator` `_DisposeMusicPlayer` `_DisposeMusicSequence` `_ExtAudioFileDispose` `_ExtAudioFileGetProperty` `_ExtAudioFileRead` `_ExtAudioFileSeek` `_ExtAudioFileSetProperty` `_ExtAudioFileWrapAudioFileID` `_MusicEventIteratorGetEventInfo` `_MusicEventIteratorHasNextEvent` `_MusicEventIteratorNextEvent` `_MusicPlayerGetBeatsForHostTime` `_MusicPlayerGetHostTimeForBeats` `_MusicPlayerGetPlayRateScalar` `_MusicPlayerGetTime` `_MusicPlayerPreroll` `_MusicPlayerSetPlayRateScalar` `_MusicPlayerSetSequence` `_MusicPlayerSetTime` `_MusicPlayerStart` `_MusicPlayerStop` `_MusicSequenceBarBeatTimeToBeats` `_MusicSequenceBeatsToBarBeatTime` `_MusicSequenceDisposeTrack` `_MusicSequenceFileCreateData` `_MusicSequenceFileLoad` `_MusicSequenceFileLoadData` `_MusicSequenceGetBeatsForSeconds` `_MusicSequenceGetIndTrack` `_MusicSequenceGetSecondsForBeats` `_MusicSequenceGetTempoTrack` `_MusicSequenceGetTrackCount` `_MusicSequenceGetTrackIndex` `_MusicSequenceSetMIDIEndpoint` `_MusicSequenceSetUserCallback` `_MusicTrackGetProperty` `_MusicTrackGetSequence` `_MusicTrackNewAUPresetEvent` `_MusicTrackNewExtendedNoteEvent` `_MusicTrackNewExtendedTempoEvent` `_MusicTrackNewMIDIChannelEvent` `_MusicTrackNewMIDINoteEvent` `_MusicTrackNewMIDIRawDataEvent` `_MusicTrackNewParameterEvent` `_MusicTrackNewUserEvent` `_NewMusicEventIterator` `_NewMusicPlayer` `_NewMusicSequence`
- **CoreMIDI** (35, SnoizeMIDI,Sparkle-helper): `_MIDIDestinationCreate` `_MIDIDeviceGetEntity` `_MIDIDeviceGetNumberOfEntities` `_MIDIEndpointDispose` `_MIDIEndpointGetEntity` `_MIDIEntityGetDestination` `_MIDIEntityGetDevice` `_MIDIEntityGetNumberOfDestinations` `_MIDIEntityGetNumberOfSources` `_MIDIEntityGetSource` `_MIDIFlushOutput` `_MIDIGetDevice` `_MIDIGetExternalDevice` `_MIDIGetNumberOfDevices` `_MIDIGetNumberOfExternalDevices` `_MIDIObjectFindByUniqueID` `_MIDIObjectGetDataProperty` `_MIDIObjectGetProperties` `_MIDIObjectSetIntegerProperty` `_MIDIObjectSetStringProperty` `_MIDIPortDisconnectSource` `_MIDIPortDispose` `_MIDIReceived` `_MIDISendSysex` `_MIDISourceCreate` `_kMIDIPropertyAdvanceScheduleTimeMuSec` `_kMIDIPropertyConnectionUniqueID` `_kMIDIPropertyDriverOwner` `_kMIDIPropertyImage` `_kMIDIPropertyMaxSysExSpeed` `_kMIDIPropertyModel` `_kMIDIPropertyName` `_kMIDIPropertyOffline` `_kMIDIPropertyPrivate` `_kMIDIPropertySingleRealtimeEntity`
- **CoreMedia** (22, Sparkle-helper): `_CMAudioFormatDescriptionGetChannelLayout` `_CMAudioFormatDescriptionGetStreamBasicDescription` `_CMFormatDescriptionGetExtensions` `_CMSampleBufferCallForEachSample` `_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer` `_CMSampleBufferGetDuration` `_CMSampleBufferGetNumSamples` `_CMSampleBufferIsValid` `_CMTimeAdd` `_CMTimeCompare` `_CMTimeCopyDescription` `_CMTimeMakeWithSeconds` `_CMTimeMultiply` `_CMTimeMultiplyByFloat64` `_CMTimeRangeGetEnd` `_CMTimeRangeMake` `_CMTimeSubtract` `_CMVideoFormatDescriptionGetPresentationDimensions` `_kCMFormatDescriptionExtension_ContainsAlphaChannel` `_kCMSampleAttachmentKey_DependsOnOthers` `_kCMTimePositiveInfinity` `_kCMTimeZero`
- **AVFoundation** (16, Sparkle-helper): `_AVCaptureDeviceTypeBuiltInWideAngleCamera` `_AVCaptureDeviceTypeExternalUnknown` `_AVCaptureSessionPresetHigh` `_AVFormatIDKey` `_AVLinearPCMBitDepthKey` `_AVLinearPCMIsBigEndianKey` `_AVLinearPCMIsFloatKey` `_AVLinearPCMIsNonInterleaved` `_AVMetadataCommonKeyAlbumName` `_AVMetadataCommonKeyArtist` `_AVMetadataCommonKeyCopyrights` `_AVMetadataCommonKeyPublisher` `_AVMetadataCommonKeyTitle` `_AVNumberOfChannelsKey` `_AVSampleRateKey` `_AVURLAssetPreferPreciseDurationAndTimingKey`
- **CoreVideo** (15, Sparkle-helper): `_CVBufferRelease` `_CVBufferRetain` `_CVGetCurrentHostTime` `_CVGetHostClockFrequency` `_CVMetalTextureCacheCreate` `_CVMetalTextureCacheCreateTextureFromImage` `_CVMetalTextureCacheFlush` `_CVMetalTextureGetTexture` `_CVPixelBufferCreateWithBytes` `_CVPixelBufferPoolRelease` `_CVPixelBufferRetain` `_kCVImageBufferPixelAspectRatioHorizontalSpacingKey` `_kCVImageBufferPixelAspectRatioKey` `_kCVImageBufferPixelAspectRatioVerticalSpacingKey` `_kCVPixelBufferCGBitmapContextCompatibilityKey`
- **Accelerate** (9, Sparkle-helper): `_cblas_dgemm` `_dgesv_` `_vDSP_maxv` `_vDSP_rmsqv` `_vDSP_vclr` `_vDSP_vlint` `_vDSP_vmaxmg` `_vDSP_vmmsb` `_vDSP_vramp`
- **AudioUnit** (6, Sparkle-helper): `_AudioComponentCopyName` `_AudioComponentGetDescription` `_AudioUnitAddRenderNotify` `_AudioUnitGetParameter` `_AudioUnitRemoveRenderNotify` `_AudioUnitSetParameter`
- **VideoToolbox** (1, Sparkle-helper): `_VTDecompressionSessionDecodeFrameWithOutputHandler`

Totals: 0 dynamic, 12 classes, 60 selectors, 166 C symbols, 0 nib entries.


---

## G6: Graphics: QuartzCore, CoreImage, OpenGL, Metal, Swift overlays, WebKit

**Scope.**
* **QuartzCore.** CAMediaTimingFunction (dynamic gap #1), CABasicAnimation
  and CAAnimation (`CAMediaTiming`; animations may apply their final value at
  once, applied through the presenter), CAGradientLayer, `-[CALayer
  addAnimation:forKey:]`, `insertSublayer:above:` and `cornerCurve`, the
  `CATransform3D*` functions, and the `kCA*` constants.
* **CoreImage.** CIImage, CIFilter (subclassed by QLab), CIContext, CIKernel,
  CIVector, CIColor, CIRenderDestination, and the built-in filters that the
  `.F53VFX` effects and `CIColorInvert` name. Fail explicitly where a filter
  or the AIR `.metallib` kernels are not supported.
* **Metal.** MTKTextureLoader and the `MTLDevice` additions:
  `newDefaultLibraryWithBundle:error:`, `newTextureWithDescriptor:iosurface:plane:`
  and `MTLCopyAllDevicesWithObserver`, plus `CGDirectDisplayCopyCurrentMetalDevice`
  and the CGDisplay serial and model numbers.
* **OpenGL, CGL and GLKit** for Syphon (40 symbols). The Metal path is the one
  QLab uses, so explicit failure is acceptable for the GL server.
* **CryptoKit** (24 Swift symbols: ChaChaPoly, P521 key agreement, HKDF over
  SHA512) and the libswiftDispatch, libswiftOSLog and CryptoKit overlay
  symbols. Use the Swift runtime build (`ALCHEMY_SWIFT.md`).
* **WebKit.** WKWebView, WKWebViewConfiguration, WKPreferences,
  WKProcessPool, WKUserContentController, WKUserScript, WKWebsiteDataStore,
  WKFlippedView, legacy WebView and WebPreferences. They are used only by the
  changelog window and Sparkle. Make them real classes that fail explicitly
  on load.
* **AppKit drawing additions.** NSGradient, NSShadow, NSAffineTransform,
  NSColor HSB/pattern/CIColor, NSImage symbol images and
  `imageWithSize:flipped:drawingHandler:`, and NSImageSymbolConfiguration.
  Also the nib NSColor color spaces that the loader rejects today.

**Owns.** `src/alchemy/kit/completion/graphics/`, `cmake/gui/qlab_graphics.cmake`,
and exclusive edits to:
* `src/alchemy/quartzcore/*`, `src/alchemy/metal/*` and
  `src/alchemy/quartz2d/*`;
* `src/alchemy/appkit/{graphics,drawing,image_codec,resources,asset_catalog}.*`;
* `src/alchemy/imageio/*` and `src/alchemy/fwgaps/graphics.cpp`;
* `scripts/build_alchemy_swift.ps1`.

**Tests.** Unit tests for:
* the timing-function curve evaluation;
* gradient interpolation;
* the CATransform3D math;
* the CIVector and CIColor value semantics;
* ChaChaPoly and HKDF against RFC 8439 and RFC 5869 vectors;
* NSAffineTransform.

### G6 item list

Dynamic (first-seen order of the pre-EULA probe run):

1. `+[CAMediaTimingFunction functionWithName:]`

Missing classes (`*` subclassed by the app, `nib` only instantiated from nibs, `helper` Sparkle helper processes only):
CAMediaTimingFunction, *CIFilter, CABasicAnimation, CAGradientLayer, CIColor, CIColorInvert (nib), CIContext, CIKernel, CIRenderDestination, CIVector, MTKTextureLoader, NSGradient, NSImageSymbolConfiguration, NSShadow, WKFlippedView (nib), WKPreferences (nib), WKProcessPool (nib), WKUserContentController, WKUserScript, WKWebView, WKWebViewConfiguration, WKWebsiteDataStore (nib), WebPreferences, WebView

Nib decoding (class: status, nib count):
CIColorInvert: class-missing (1), NSColor: rejected-by-nib-loader (119), NSShadow: class-missing (1), WKFlippedView: class-missing (1), WKPreferences: class-missing (1), WKProcessPool: class-missing (1), WKUserContentController: class-missing (1), WKWebView: class-missing (1), WKWebViewConfiguration: class-missing (1), WKWebsiteDataStore: class-missing (1)

Selectors not implemented by any host class, by likely owner (`h` = Sparkle helper processes only):

- **CAMediaTimingFunction**: `functionWithName:`
- **CAAnimation**: `setAutoreverses:`
- **CABasicAnimation**: `animationWithKeyPath:` `setFromValue:` `setToValue:`
- **CAGradientLayer**: `setColors:` `setEndPoint:` `setLocations:` `setStartPoint:`
- **CALayer**: `addAnimation:forKey:` `insertSublayer:above:` `setCornerCurve:` `setTransform:` `transform`
- **CIContext**: `contextWithCGContext:options:` `contextWithMTLDevice:` `contextWithMTLDevice:options:` `createCGImage:fromRect:` `render:toMTLTexture:commandBuffer:bounds:colorSpace:` `startTaskToRender:toDestination:error:`
- **CIFilter**: `filterWithName:`
- **CIImage**: `clearImage` `imageByApplyingFilter:` `imageByApplyingFilter:withInputParameters:` `imageByApplyingTransform:` `imageByColorMatchingWorkingSpaceToColorSpace:` `imageByCroppingToRect:` `imageBySamplingNearest` `imageWithCGImage:` `imageWithCVPixelBuffer:options:` `imageWithContentsOfURL:options:` `imageWithData:` `imageWithMTLTexture:options:` `initWithMTLTexture:options:`
- **CIKernel**: `applyWithExtent:roiCallback:arguments:` `kernelWithFunctionName:fromMetalLibraryData:error:`
- **CIRenderDestination**: `initWithBitmapData:width:height:bytesPerRow:format:` `initWithMTLTexture:commandBuffer:`
- **CIRenderTask**: `waitUntilCompletedAndReturnError:`
- **CIVector**: `initWithValues:count:` `vectorWithCGRect:` `vectorWithX:Y:` `vectorWithX:Y:Z:W:`
- **MTKTextureLoader**: `newTextureWithCGImage:options:error:`
- **MTLDevice**: `newDefaultLibraryWithBundle:error:` `newTextureWithDescriptor:iosurface:plane:`
- **NSAffineTransform**: `concat` `rotateByRadians:` `scaleXBy:yBy:` `transformPoint:` `translateXBy:yBy:`
- **NSColor**: `colorWithCIColor:` `colorWithCalibratedHue:saturation:brightness:alpha:` `colorWithPatternImage:` `getHue:saturation:brightness:alpha:`
- **NSGradient**: `drawInBezierPath:angle:` `drawInRect:angle:` `initWithColorsAndLocations:` `initWithStartingColor:endingColor:`
- **NSGraphicsContext/CIContext**: `drawImage:inRect:fromRect:`
- **NSImage**: `imageWithSize:flipped:drawingHandler:` `imageWithSymbolConfiguration:` `imageWithSystemSymbolName:accessibilityDescription:`
- **NSImageSymbolConfiguration**: `configurationWithScale:`
- **NSShadow**: `setShadowBlurRadius:` `setShadowColor:` `setShadowOffset:` `shadowBlurRadius` `shadowOffset`
- **WKNavigationAction (policy)**: `ignore` `use`
- **WKUserContentController**: `addUserScript:`
- **WKUserScript**: `initWithSource:injectionTime:forMainFrameOnly:`
- **WKWebView**: `initWithFrame:configuration:` `loadData:MIMEType:characterEncodingName:baseURL:` `loadFileURL:allowingReadAccessToURL:` `setNavigationDelegate:`
- **WKWebView (private)**: `_setDrawsBackground:`
- **WKWebView/WebFrame**: `loadHTMLString:baseURL:`
- **WKWebViewConfiguration**: `setUserContentController:`
- **WebFrame**: `loadData:MIMEType:textEncodingName:baseURL:` `parentFrame`
- **WebPreferences**: `setAllowsAnimatedImageLooping:` `setAllowsAnimatedImages:` `setDefaultFontSize:` `setJavaEnabled:` `setJavaScriptCanOpenWindowsAutomatically:` `setJavaScriptEnabled:` `setLoadsImagesAutomatically:` `setPlugInsEnabled:` `setStandardFontFamily:` `setUserStyleSheetEnabled:` `setUserStyleSheetLocation:`
- **WebView**: `mainFrame` `setFrameLoadDelegate:` `setPolicyDelegate:` `stopLoading:`
- **WebView (private)**: `_setDrawsTransparentBackground:`
- **WebView/WKWebView**: `setUIDelegate:`
- **WebView/WKWebViewConfiguration**: `setPreferences:`

Unresolved strong C imports (doctor), by library:

- **OpenGL** (40, Syphon): `_CGLGetPixelFormat` `_glAttachShader` `_glBindBuffer` `_glBindFramebuffer` `_glBindRenderbuffer` `_glBindTexture` `_glBindVertexArray` `_glBlitFramebuffer` `_glBufferData` `_glCheckFramebufferStatus` `_glCompileShader` `_glCreateProgram` `_glCreateShader` `_glDeleteBuffers` `_glDeleteFramebuffers` `_glDeleteProgram` `_glDeleteRenderbuffers` `_glDeleteShader` `_glDeleteTextures` `_glDeleteVertexArrays` `_glDrawArrays` `_glEnableVertexAttribArray` `_glFlush` `_glFlushRenderAPPLE` `_glFramebufferRenderbuffer` `_glFramebufferTexture2D` `_glGenBuffers` `_glGenFramebuffers` `_glGenRenderbuffers` `_glGenTextures` `_glGenVertexArrays` `_glGetAttribLocation` `_glGetProgramiv` `_glGetShaderiv` `_glLinkProgram` `_glRenderbufferStorageMultisample` `_glShaderSource` `_glUseProgram` `_glVertexAttribPointer` `_glViewport`
- **CryptoKit** (24, Sparkle-helper): `_$s9CryptoKit03ChaC4PolyO4open_5using10Foundation4DataVAC9SealedBoxV_AA12SymmetricKeyVtKFZ` `_$s9CryptoKit03ChaC4PolyO4seal_5using5nonceAC9SealedBoxVx_AA12SymmetricKeyVAC5NonceVSgtK10Foundation12DataProtocolRzlFZ` `_$s9CryptoKit03ChaC4PolyO5NonceVMa` `_$s9CryptoKit03ChaC4PolyO5NonceVMn` `_$s9CryptoKit0aB5ErrorO22incorrectParameterSizeyA2CmFWC` `_$s9CryptoKit0aB5ErrorOMa` `_$s9CryptoKit0aB5ErrorOs0C0AAMc` `_$s9CryptoKit12SharedSecretV23hkdfDerivedSymmetricKey5using4salt10sharedInfo15outputByteCountAA0gH0Vxm_q_q0_SitAA12HashFunctionRz10Foundation12DataProtocolR_AlMR0_r1_lF` `_$s9CryptoKit12SharedSecretVMa` `_$s9CryptoKit12SharedSecretVMn` `_$s9CryptoKit12SymmetricKeyVMa` `_$s9CryptoKit12SymmetricKeyVMn` `_$s9CryptoKit4P521O12KeyAgreementO06PublicD0V17rawRepresentation10Foundation4DataVvg` `_$s9CryptoKit4P521O12KeyAgreementO06PublicD0V17rawRepresentationAGx_tKc10Foundation15ContiguousBytesRzlufC` `_$s9CryptoKit4P521O12KeyAgreementO06PublicD0VMa` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0V016sharedSecretFromdE04withAA06SharedH0VAE06PublicD0V_tKF` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0V06publicD0AE06PublicD0Vvg` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0V17rawRepresentation10Foundation4DataVvg` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0V17rawRepresentationAGx_tKc10Foundation15ContiguousBytesRzlufC` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0V20compactRepresentableAGSb_tcfC` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0VMa` `_$s9CryptoKit4P521O12KeyAgreementO07PrivateD0VMn` `_$s9CryptoKit6SHA512VAA12HashFunctionAAMc` `_$s9CryptoKit6SHA512VMa`
- **libswiftDispatch.dylib** (14, Sparkle-helper): `_$s8Dispatch0A13WorkItemFlagsVMa` `_$s8Dispatch0A13WorkItemFlagsVMn` `_$s8Dispatch0A13WorkItemFlagsVs10SetAlgebraAAMc` `_$s8Dispatch0A3QoSV10backgroundACvgZ` `_$s8Dispatch0A3QoSV11unspecifiedACvgZ` `_$s8Dispatch0A3QoSVMa` `_$sSo17OS_dispatch_queueC8DispatchE10AttributesVMa` `_$sSo17OS_dispatch_queueC8DispatchE10AttributesVMn` `_$sSo17OS_dispatch_queueC8DispatchE10AttributesVs10SetAlgebraACMc` `_$sSo17OS_dispatch_queueC8DispatchE20AutoreleaseFrequencyO7inherityA2EmFWC` `_$sSo17OS_dispatch_queueC8DispatchE20AutoreleaseFrequencyOMa` `_$sSo17OS_dispatch_queueC8DispatchE4mainABvgZ` `_$sSo17OS_dispatch_queueC8DispatchE5async5group3qos5flags7executeySo0a1_b1_F0CSg_AC0D3QoSVAC0D13WorkItemFlagsVyyXBtF` `_$sSo17OS_dispatch_queueC8DispatchE5label3qos10attributes20autoreleaseFrequency6targetABSS_AC0D3QoSVAbCE10AttributesVAbCE011AutoreleaseI0OABSgtcfC`
- **QuartzCore** (11, Sparkle-helper): `_CATransform3DConcat` `_CATransform3DMakeScale` `_CATransform3DRotate` `_CATransform3DScale` `_CATransform3DTranslate` `_kCACornerCurveCircular` `_kCAGravityCenter` `_kCAGravityResize` `_kCAMediaTimingFunctionDefault` `_kCAMediaTimingFunctionEaseIn` `_kCAMediaTimingFunctionEaseInEaseOut`
- **CoreImage** (9, Sparkle-helper): `_kCIAttributeFilterDisplayName` `_kCIContextWorkingColorSpace` `_kCIFormatBGRA8` `_kCIImageApplyOrientationProperty` `_kCIImageColorSpace` `_kCIInputBackgroundImageKey` `_kCIInputBrightnessKey` `_kCIInputContrastKey` `_kCIInputImageKey`
- **CoreGraphics** (4, Sparkle-helper): `_CGDirectDisplayCopyCurrentMetalDevice` `_CGDisplayModelNumber` `_CGDisplaySerialNumber` `_CGDisplayVendorNumber`
- **Metal** (4, Sparkle-helper): `_MTLCopyAllDevicesWithObserver` `_MTLDeviceRemovalRequestedNotification` `_MTLDeviceWasAddedNotification` `_MTLDeviceWasRemovedNotification`
- **GLKit** (3, Sparkle-helper): `_GLKMatrix4Identity` `_GLKQuaternionIdentity` `_GLKQuaternionSlerp`
- **MetalKit** (2, Sparkle-helper): `_MTKTextureLoaderOptionGenerateMipmaps` `_MTKTextureLoaderOptionSRGB`
- **libswiftOSLog.dylib** (1, Sparkle-helper): `_$sSo10OSLogStoreC0A0E10getEntries4with2at8matchings11AnySequenceVySo0A5EntryCGSo0A17EnumeratorOptionsV_So0A8PositionCSgSo11NSPredicateCSgtKF`

Totals: 1 dynamic, 24 classes, 100 selectors, 112 C symbols, 10 nib entries.


---

## Not gaps

* **328 delegate and data-source selectors.** These are the selectors that
  AppKit, Foundation and Sparkle send to the app
  (`tableView:…`, `outlineView:…`, `windowDid…`, `URLSession:…`, `updater…`).
  They are in `gaps.json` under `callback_selectors`. They matter only when
  the host must *send* them, which is each owning class's job.
* **Selectors the app defines** (30,904 methods in the main executable) and
  its 27 categories on system classes.
* **Sparkle's helper processes.** Their entries are marked `h` and `(helper)`.
  They run as separate processes and are not needed to reach the main
  window.
