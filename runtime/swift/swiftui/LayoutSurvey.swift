// SwiftUI for Alchemy: the layout survey (ALCHEMY_SWIFTUI_LAYOUTS=report or probe).
//
// Apps compiled against Apple's SwiftUI lay out their own structs and classes around the stored layouts of the
// types Apple declares @frozen, and their Swift metadata records the result: a struct whose fields all have a layout
// known when it was compiled carries static field offsets. The survey reads those offsets from the app's own
// metadata, through the open-source Swift runtime ABI only (the __swift5_types records, context descriptors, metadata
// accessors, the runtime's field enumeration, and value-witness sizes), and reports, for every field typed as one of
// those SwiftUI types, the room the app gives it.
//
// Sizes do not say which word of a type holds what. The probe answers that from the app's own compiled value
// witnesses: for an app struct whose only @frozen SwiftUI field is the type probed and whose other fields are plain
// data or class references, it places a probe object in one word of that field at a time (every other byte zero),
// copies the struct with the app's initializeWithCopy witness, destroys the copy with its destroy witness, and reports
// which words were retained and released, and how (Swift or Objective-C reference counting). For a one-field struct it
// also stores and reads back an Optional's tags with the app's enum-tag witnesses, to see which word supplies the
// extra inhabitants. It calls no app function other than metadata accessors and those value witnesses.

import Darwin
import ObjectiveC
@_spi(Reflection) import Swift

/// The types Apple documents as @frozen, by unqualified name.
internal let _frozenSwiftUITypes: Set<String> = [
    "State", "Binding", "Text", "Color", "AnyView", "Transaction", "Environment", "ModifiedContent", "TupleView",
    "EdgeInsets", "Spacer", "Image", "ObservedObject", "EnvironmentObject", "FocusState", "EmptyView", "HStack", "VStack",
]

/// The objects the probe places in a value's words; their counts show what the app's witnesses did. An Objective-C
/// retain or release changes the count the Objective-C runtime keeps (-retainCount). A Swift one changes a native
/// Swift object's inline reference counts, the word after the isa (InlineRefCounts; strong extra count from bit 33),
/// which here is `swiftCounts`. Its 0 is a native object's single strong reference, and the probe balances every
/// retain with a release, so neither kind ever deallocates the object.
internal final class _LayoutProbeObject: NSObject {
    var swiftCounts: UInt64 = 0
}

internal enum _LayoutSurvey {
    /// Runs the survey when ALCHEMY_SWIFTUI_LAYOUTS asks for it (`report` or `probe`); any other value stops.
    static func runIfRequested() {
        guard let setting = getenv("ALCHEMY_SWIFTUI_LAYOUTS") else { return }
        let mode = String(cString: setting)
        switch mode {
        case "report": report()
        case "probe": probe()
        default: fatalError("SwiftUI: ALCHEMY_SWIFTUI_LAYOUTS=\(mode) is not a survey mode (report, probe)")
        }
    }

    private static func emit(_ line: String) { fputs("SwiftUI layout: " + line + "\n", __stderrp) }

    /// The value witness table of a type's metadata (the word before its address point).
    private static func valueWitnesses(_ metadata: UnsafeRawPointer) -> UnsafeRawPointer {
        return metadata.load(fromByteOffset: -8, as: UnsafeRawPointer.self)
    }

    /// size, alignment and the value witness flags of a type.
    private static func extent(_ type: Any.Type) -> (size: Int, alignment: Int, flags: UInt32, extraInhabitants: UInt32) {
        let witnesses = valueWitnesses(unsafeBitCast(type, to: UnsafeRawPointer.self))
        let flags = witnesses.load(fromByteOffset: 80, as: UInt32.self)
        return (witnesses.load(fromByteOffset: 64, as: Int.self), Int(flags & 0xff) + 1, flags,
                witnesses.load(fromByteOffset: 84, as: UInt32.self))
    }

    /// Value witness flags: the type is not plain data (copying it does more than copy its bytes).
    private static let nonPOD: UInt32 = 0x10000

    /// The unqualified name of a SwiftUI frozen type a field has, or nil.
    private static func frozenName(_ type: Any.Type) -> String? {
        let name = _typeName(type, qualified: true)
        guard name.hasPrefix("SwiftUI.") else { return nil }
        let base = String(name.dropFirst("SwiftUI.".count).prefix { $0 != "<" && $0 != "." })
        return _frozenSwiftUITypes.contains(base) ? base : nil
    }

    private typealias Field = (name: String, offset: Int, type: Any.Type)

    /// Calls `body` with every non-generic struct of the main executable that has a field of a @frozen SwiftUI type:
    /// its metadata, whether the compiler laid it out (static field offsets), and its fields by offset. Returns the
    /// number of type records.
    @discardableResult
    private static func forEachStruct(_ body: (_ type: Any.Type, _ isStatic: Bool, _ fields: [Field]) -> Void) -> Int? {
        guard let header = _dyld_get_image_header(0) else { emit("no main executable"); return nil }
        var size: UInt = 0
        let machHeader = UnsafeRawPointer(header).assumingMemoryBound(to: mach_header_64.self)
        guard let section = getsectiondata(machHeader, "__TEXT", "__swift5_types", &size) else {
            emit("the main executable has no __swift5_types")
            return nil
        }
        let records = UnsafeRawPointer(section)
        for index in 0 ..< Int(size) / 4 {
            // A TypeMetadataRecord: a 32-bit relative pointer whose low two bits say how it refers to its descriptor.
            let record = records + index * 4
            let value = Int(record.load(as: Int32.self))
            let target = record + (value & ~3)
            let descriptor: UnsafeRawPointer?
            switch value & 3 {
            case 0: descriptor = target
            case 1: descriptor = target.load(as: UnsafeRawPointer?.self)
            default: descriptor = nil   // Objective-C class references
            }
            guard let descriptor else { continue }
            let flags = descriptor.load(as: UInt32.self)
            // Structs only (kind 17): SwiftUI's values live in views, which are structs. Not generic (0x80).
            guard flags & 0x1f == 17, flags & 0x80 == 0 else { continue }
            // Kind-specific flags: metadata initialization 0 means the compiler laid the type out (static field
            // offsets, which is the evidence); 1 means the runtime does, from the field types' sizes in this process.
            let isStatic = (flags >> 16) & 3 == 0
            let accessorOffset = Int(descriptor.load(fromByteOffset: 12, as: Int32.self))
            typealias Accessor = @convention(c) (Int) -> UnsafeRawPointer
            let metadata = unsafeBitCast(descriptor + 12 + accessorOffset, to: Accessor.self)(0)   // MetadataRequest complete
            let type = unsafeBitCast(metadata, to: Any.Type.self)
            var fields: [Field] = []
            _ = _forEachField(of: type, options: []) { name, offset, fieldType, _ in
                fields.append((String(cString: name), offset, fieldType))
                return true
            }
            guard fields.contains(where: { frozenName($0.type) != nil }) else { continue }
            body(type, isStatic, fields.sorted { $0.offset < $1.offset })
        }
        return Int(size) / 4
    }

    // MARK: - report

    static func report() {
        var surveyed = 0
        guard let records = forEachStruct({ type, isStatic, fields in
            let container = extent(type)
            emit("container \(_typeName(type, qualified: true)) size \(container.size) align \(container.alignment)"
                 + " layout \(isStatic ? "static" : "runtime") flags 0x\(String(container.flags, radix: 16)) xi \(container.extraInhabitants)")
            for (index, field) in fields.enumerated() {
                let end = index + 1 < fields.count ? fields[index + 1].offset : container.size
                let ours = extent(field.type)
                let frozen = frozenName(field.type)
                if frozen != nil { surveyed += 1 }
                emit("  field \(field.name) offset \(field.offset) room \(end - field.offset) type \(_typeName(field.type, qualified: true))"
                     + (frozen != nil ? " [frozen \(frozen!); ours size \(ours.size) align \(ours.alignment)]" : " size \(ours.size)"))
            }
        }) else { return }
        emit("done: \(records) type records, \(surveyed) fields of @frozen SwiftUI types")
    }

    // MARK: - probe

    /// The @frozen types the probe handles: those whose words it may fill with probe objects one at a time, the
    /// other words zero.
    private static let probedTypes: Set<String> = ["ObservedObject", "Binding"]

    /// The Objective-C runtime's retain count of a probe object (-retainCount through its implementation).
    private static func objectiveCCount(_ object: _LayoutProbeObject) -> Int {
        typealias RetainCount = @convention(c) (AnyObject, Selector) -> UInt
        guard let selector = sel_registerName("retainCount"),
              let implementation = class_getMethodImplementation(object_getClass(object), selector) else { return -1 }
        return Int(unsafeBitCast(implementation, to: RetainCount.self)(object, selector))
    }

    /// A change of the probe object's Swift strong count, from its inline counts.
    private static func swiftDelta(_ before: UInt64, _ after: UInt64) -> Int {
        return Int(Int64(bitPattern: after &- before) >> 33)
    }

    /// Which word of a one-field struct supplies its extra inhabitants: the word an enum of one empty case (an
    /// Optional) writes for that case. With every word of a buffer holding a probe object's address (a valid
    /// reference, and valid plain data), it stores the payload case and reads the tag back, then stores the empty
    /// case and reports the words that changed and the tag read back, through the app's own
    /// storeEnumTagSinglePayload and getEnumTagSinglePayload witnesses. Nothing is retained or released.
    private static func probeTags(_ type: Any.Type, size: Int) {
        let metadata = unsafeBitCast(type, to: UnsafeRawPointer.self)
        let witnesses = valueWitnesses(metadata)
        typealias GetTag = @convention(c) (UnsafeRawPointer, UInt32, UnsafeRawPointer) -> UInt32
        typealias StoreTag = @convention(c) (UnsafeMutableRawPointer, UInt32, UInt32, UnsafeRawPointer) -> Void
        let getTag = unsafeBitCast(witnesses.load(fromByteOffset: 48, as: UnsafeRawPointer.self), to: GetTag.self)
        let storeTag = unsafeBitCast(witnesses.load(fromByteOffset: 56, as: UnsafeRawPointer.self), to: StoreTag.self)
        let object: _LayoutProbeObject = _LayoutProbeObject()
        let address = UInt(bitPattern: Unmanaged.passUnretained(object).toOpaque())
        // Room for tag bytes past the value, which one empty case should never need; they are reported if written.
        let words = size / 8, spare = 2
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: (words + spare) * 8, alignment: 16)
        func fill() { for word in 0 ..< words + spare { buffer.storeBytes(of: address, toByteOffset: 8 * word, as: UInt.self) } }
        func changed() -> String {
            let words = (0 ..< words + spare).compactMap { word -> String? in
                let value = buffer.load(fromByteOffset: 8 * word, as: UInt.self)
                return value == address ? nil : "word \(word) = 0x\(String(value, radix: 16))"
            }
            return words.isEmpty ? "no word changed" : words.joined(separator: ", ")
        }
        fill()
        storeTag(buffer, 0, 1, metadata)
        emit("  tag payload: stored case 0, read back \(getTag(buffer, 1, metadata)), \(changed())")
        fill()
        storeTag(buffer, 1, 1, metadata)
        emit("  tag empty: stored case 1 of 1, read back \(getTag(buffer, 1, metadata)), \(changed())")
        buffer.deallocate()
        withExtendedLifetime(object) {}
    }

    static func probe() {
        // The probe objects' Swift counts must sit where a native object keeps them: the word after the isa.
        guard let ivar = class_getInstanceVariable(_LayoutProbeObject.self, "swiftCounts"), ivar_getOffset(ivar) == 8 else {
            emit("probe: the probe objects' swiftCounts is not at offset 8; nothing probed")
            return
        }
        var probed = 0
        forEachStruct { type, isStatic, fields in
            // Evidence needs the compiler's own layout and copy code for exactly one field of a probed type.
            guard isStatic else { return }
            let frozen = fields.filter { frozenName($0.type) != nil }
            guard frozen.count == 1, let field = frozen.first, let name = frozenName(field.type), probedTypes.contains(name) else { return }
            let label = "probe \(_typeName(type, qualified: true)).\(field.name) \(_typeName(field.type, qualified: true))"
            // Every other field must be plain data or a class reference, so that zero bytes are a valid value of it
            // (a null reference, which retain and release ignore) and its copy touches no probe object.
            if let other = fields.first(where: { $0.offset != field.offset && extent($0.type).flags & nonPOD != 0 && !($0.type is AnyObject.Type) }) {
                emit("\(label): skipped, field \(other.name) is neither plain data nor a class reference")
                return
            }
            let container = extent(type)
            // The room the app's layout gives the field (up to the next field or the end of the struct), not our size.
            let fieldSize = (fields.first(where: { $0.offset > field.offset })?.offset ?? container.size) - field.offset
            let witnesses = valueWitnesses(unsafeBitCast(type, to: UnsafeRawPointer.self))
            typealias InitializeWithCopy = @convention(c) (UnsafeMutableRawPointer, UnsafeMutableRawPointer, UnsafeRawPointer) -> UnsafeMutableRawPointer
            typealias Destroy = @convention(c) (UnsafeMutableRawPointer, UnsafeRawPointer) -> Void
            let initializeWithCopy = unsafeBitCast(witnesses.load(fromByteOffset: 16, as: UnsafeRawPointer.self), to: InitializeWithCopy.self)
            let destroy = unsafeBitCast(witnesses.load(fromByteOffset: 8, as: UnsafeRawPointer.self), to: Destroy.self)
            let metadata = unsafeBitCast(type, to: UnsafeRawPointer.self)
            emit("\(label) at offset \(field.offset), size \(fieldSize)")
            for word in 0 ..< fieldSize / 8 {
                let source = UnsafeMutableRawPointer.allocate(byteCount: container.size, alignment: 16)
                let copy = UnsafeMutableRawPointer.allocate(byteCount: container.size, alignment: 16)
                source.initializeMemory(as: UInt8.self, repeating: 0, count: container.size)
                copy.initializeMemory(as: UInt8.self, repeating: 0xa5, count: container.size)
                let object: _LayoutProbeObject = _LayoutProbeObject()
                source.storeBytes(of: Unmanaged.passUnretained(object).toOpaque(), toByteOffset: field.offset + 8 * word, as: UnsafeMutableRawPointer.self)
                // Emitted first, so that a copy that stops names the word it stopped on.
                emit("  word \(word):")
                let swiftBefore = object.swiftCounts, objcBefore = objectiveCCount(object)
                _ = initializeWithCopy(copy, source, metadata)
                let swiftCopied = object.swiftCounts, objcCopied = objectiveCCount(object)
                let same = memcmp(copy + field.offset, source + field.offset, fieldSize) == 0
                destroy(copy, metadata)
                let swiftDestroyed = object.swiftCounts, objcDestroyed = objectiveCCount(object)
                emit("  word \(word): copy swift \(swiftDelta(swiftBefore, swiftCopied)) objc \(objcCopied - objcBefore),"
                     + " destroy swift \(swiftDelta(swiftCopied, swiftDestroyed)) objc \(objcDestroyed - objcCopied), bytes \(same ? "same" : "differ")")
                source.deallocate()
                copy.deallocate()
                withExtendedLifetime(object) {}
            }
            if fields.count == 1 { probeTags(type, size: container.size) }
            probed += 1
        }
        emit("probe done: \(probed) fields probed")
    }
}
