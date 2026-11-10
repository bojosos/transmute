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
// data or class references, it places a probe object in one word of that field at a time (every other byte zero), copies the struct with
// the app's initializeWithCopy witness, destroys the copy with its destroy witness, and reports which words were
// retained and released. It calls no app function other than metadata accessors and those two witnesses.

import Darwin
@_spi(Reflection) import Swift

/// The types Apple documents as @frozen, by unqualified name.
internal let _frozenSwiftUITypes: Set<String> = [
    "State", "Binding", "Text", "Color", "AnyView", "Transaction", "Environment", "ModifiedContent", "TupleView",
    "EdgeInsets", "Spacer", "Image", "ObservedObject", "EnvironmentObject", "FocusState", "EmptyView", "HStack", "VStack",
]

/// The objects the probe places in a value's words; their retain counts show what the app's witnesses did.
internal final class _LayoutProbeObject {}

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

    /// The @frozen types the probe handles: those whose words it may fill with probe objects. ObservedObject holds
    /// an object reference and other data; its probe object stands in for the observed object, so the observed
    /// object's class must use Swift reference counting, as the probe objects do.
    private static let probedTypes: Set<String> = ["ObservedObject"]

    static func probe() {
        var probed = 0
        forEachStruct { type, isStatic, fields in
            // Evidence needs the compiler's own layout and copy code for exactly one field of a probed type.
            guard isStatic else { return }
            let frozen = fields.filter { frozenName($0.type) != nil }
            guard frozen.count == 1, let field = frozen.first, let name = frozenName(field.type), probedTypes.contains(name) else { return }
            let containerName = _typeName(type, qualified: true)
            let fieldTypeName = _typeName(field.type, qualified: true)
            // Every other field must be plain data or a class reference, so that zero bytes are a valid value of it
            // (a null reference, which retain and release ignore) and its copy touches no probe object.
            if let other = fields.first(where: { $0.offset != field.offset && extent($0.type).flags & nonPOD != 0 && !($0.type is AnyObject.Type) }) {
                emit("probe \(containerName).\(field.name) \(fieldTypeName): skipped, field \(other.name) is neither plain data nor a class reference")
                return
            }
            if name == "ObservedObject" {
                // The generic argument (ObjectType) follows the struct metadata's kind and description words.
                let object = unsafeBitCast(field.type, to: UnsafeRawPointer.self).load(fromByteOffset: 16, as: UnsafeRawPointer.self)
                // Class metadata (Objective-C interop layout): isa, superclass, two cache words, data, then the Swift
                // class flags; UsesSwiftRefcounting is 0x2.
                let classFlags = object.load(fromByteOffset: 40, as: UInt32.self)
                guard classFlags & 0x2 != 0 else {
                    emit("probe \(containerName).\(field.name) \(fieldTypeName): skipped, ObjectType uses Objective-C reference counting")
                    return
                }
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
            var words: [String] = []
            for word in 0 ..< fieldSize / 8 {
                let source = UnsafeMutableRawPointer.allocate(byteCount: container.size, alignment: 16)
                let copy = UnsafeMutableRawPointer.allocate(byteCount: container.size, alignment: 16)
                source.initializeMemory(as: UInt8.self, repeating: 0, count: container.size)
                copy.initializeMemory(as: UInt8.self, repeating: 0xa5, count: container.size)
                let object = _LayoutProbeObject()
                source.storeBytes(of: Unmanaged.passUnretained(object).toOpaque(), toByteOffset: field.offset + 8 * word, as: UnsafeMutableRawPointer.self)
                let before = _getRetainCount(object)
                _ = initializeWithCopy(copy, source, metadata)
                let copied = _getRetainCount(object)
                let same = memcmp(copy + field.offset, source + field.offset, fieldSize) == 0
                destroy(copy, metadata)
                let destroyed = _getRetainCount(object)
                words.append("word \(word): copy \(Int(copied) - Int(before)), destroy \(Int(destroyed) - Int(copied)), bytes \(same ? "same" : "differ")")
                source.deallocate()
                copy.deallocate()
                withExtendedLifetime(object) {}
            }
            probed += 1
            emit("probe \(containerName).\(field.name) \(fieldTypeName) at offset \(field.offset), size \(fieldSize): " + words.joined(separator: "; "))
        }
        emit("probe done: \(probed) fields probed")
    }
}
