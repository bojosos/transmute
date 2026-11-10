// SwiftUI for Alchemy: the layout survey (ALCHEMY_SWIFTUI_LAYOUTS=report).
//
// Apps compiled against Apple's SwiftUI lay out their own structs and classes around the stored layouts of the
// types Apple declares @frozen, and their Swift metadata records the result: a struct whose fields all have a layout
// known when it was compiled carries static field offsets. The survey reads those offsets from the app's own
// metadata, through the open-source Swift runtime ABI only (the __swift5_types records, context descriptors, metadata
// accessors, the runtime's field enumeration, and value-witness sizes), and reports, for every field typed as one of
// those SwiftUI types, the room the app gives it. It reads data; it never calls an app function other than metadata
// accessors.

import Darwin
@_spi(Reflection) import Swift

/// The types Apple documents as @frozen, by unqualified name.
internal let _frozenSwiftUITypes: Set<String> = [
    "State", "Binding", "Text", "Color", "AnyView", "Transaction", "Environment", "ModifiedContent", "TupleView",
    "EdgeInsets", "Spacer", "Image", "ObservedObject", "EnvironmentObject", "FocusState", "EmptyView", "HStack", "VStack",
]

internal enum _LayoutSurvey {
    /// Runs the survey when ALCHEMY_SWIFTUI_LAYOUTS asks for it (`report`); any other value stops.
    static func runIfRequested() {
        guard let setting = getenv("ALCHEMY_SWIFTUI_LAYOUTS") else { return }
        let mode = String(cString: setting)
        guard mode == "report" else { fatalError("SwiftUI: ALCHEMY_SWIFTUI_LAYOUTS=\(mode) is not a survey mode (report)") }
        report()
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

    /// The unqualified name of a SwiftUI frozen type a field has, or nil.
    private static func frozenName(_ type: Any.Type) -> String? {
        let name = _typeName(type, qualified: true)
        guard name.hasPrefix("SwiftUI.") else { return nil }
        let base = String(name.dropFirst("SwiftUI.".count).prefix { $0 != "<" && $0 != "." })
        return _frozenSwiftUITypes.contains(base) ? base : nil
    }

    static func report() {
        guard let header = _dyld_get_image_header(0) else { emit("no main executable"); return }
        var size: UInt = 0
        let machHeader = UnsafeRawPointer(header).assumingMemoryBound(to: mach_header_64.self)
        guard let section = getsectiondata(machHeader, "__TEXT", "__swift5_types", &size) else {
            emit("the main executable has no __swift5_types")
            return
        }
        let records = UnsafeRawPointer(section)
        var surveyed = 0
        for index in 0 ..< Int(size) / 4 {
            // A TypeMetadataRecord: a 32-bit relative pointer whose low two bits say how it refers to its descriptor.
            let record = records + index * 4
            let value = Int(record.load(as: Int32.self))
            let target = record + (value & ~3)
            switch value & 3 {
            case 0: surveyed += survey(target)
            case 1: if let descriptor = target.load(as: UnsafeRawPointer?.self) { surveyed += survey(descriptor) }
            default: break   // Objective-C class references
            }
        }
        emit("done: \(Int(size) / 4) type records, \(surveyed) fields of @frozen SwiftUI types")
    }

    /// Reports one non-generic struct; returns how many fields of @frozen SwiftUI types it has.
    private static func survey(_ descriptor: UnsafeRawPointer) -> Int {
        let flags = descriptor.load(as: UInt32.self)
        let kind = flags & 0x1f            // 17 struct
        // Structs only: SwiftUI's values live in views, which are structs; and the runtime's field enumeration of a
        // class with Objective-C ancestry goes through the Objective-C runtime's ivar lists, which Alchemy does not
        // answer for every class yet.
        guard kind == 17, flags & 0x80 == 0 else { return 0 }   // not generic
        // Kind-specific flags: metadata initialization 0 means the compiler laid the type out (static field
        // offsets, which is the evidence); 1 means the runtime does, from the field types' sizes in this process.
        let layout = (flags >> 16) & 3 == 0 ? "static" : "runtime"
        let accessorOffset = Int(descriptor.load(fromByteOffset: 12, as: Int32.self))
        typealias Accessor = @convention(c) (Int) -> UnsafeRawPointer
        let metadata = unsafeBitCast(descriptor + 12 + accessorOffset, to: Accessor.self)(0)   // MetadataRequest complete
        let type = unsafeBitCast(metadata, to: Any.Type.self)
        var fields: [(name: String, offset: Int, type: Any.Type)] = []
        _ = _forEachField(of: type, options: []) { name, offset, fieldType, _ in
            fields.append((String(cString: name), offset, fieldType))
            return true
        }
        guard fields.contains(where: { frozenName($0.type) != nil }) else { return 0 }
        let container = extent(type)
        let containerSize = container.size
        let containerName = _typeName(type, qualified: true)
        emit("container \(containerName) size \(containerSize) align \(container.alignment) layout \(layout)"
             + " flags 0x\(String(container.flags, radix: 16)) xi \(container.extraInhabitants)")
        fields.sort { $0.offset < $1.offset }
        var count = 0
        for (index, field) in fields.enumerated() {
            let end = index + 1 < fields.count ? fields[index + 1].offset : containerSize
            let ours = extent(field.type)
            let frozen = frozenName(field.type)
            if frozen != nil { count += 1 }
            emit("  field \(field.name) offset \(field.offset) room \(end - field.offset) type \(_typeName(field.type, qualified: true))"
                 + (frozen != nil ? " [frozen \(frozen!); ours size \(ours.size) align \(ours.alignment)]" : " size \(ours.size)"))
        }
        return count
    }
}
