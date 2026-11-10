// UniformTypeIdentifiers for Alchemy: UTType as a uniform type identifier (written from Apple's public documentation;
// nothing here is Apple code). A type is its identifier string; the system's type database (conformance, tags,
// descriptions) is not consulted yet, and the members that need it stop explicitly.

/// A uniform type identifier.
public struct UTType: Hashable, Sendable, CustomStringConvertible {
    /// The type's identifier, such as "public.image".
    public let identifier: String

    internal init(identifier: String) { self.identifier = identifier }

    /// The type with the identifier.
    public init?(_ identifier: String) {
        guard !identifier.isEmpty else { return nil }
        self.identifier = identifier
    }

    /// The type an app exports with the identifier, conforming to a parent type.
    public init(exportedAs identifier: String, conformingTo parentType: UTType? = nil) {
        self.identifier = identifier
    }

    /// The type an app imports with the identifier, conforming to a parent type.
    public init(importedAs identifier: String, conformingTo parentType: UTType? = nil) {
        self.identifier = identifier
    }

    /// The type of files with the extension, among the well-known public types. Any other extension needs the
    /// system's type database, which is not consulted yet, and stops explicitly.
    public init?(filenameExtension: String, conformingTo supertype: UTType = .data) {
        let known: [String: String] = [
            "png": "public.png", "jpg": "public.jpeg", "jpeg": "public.jpeg", "tif": "public.tiff", "tiff": "public.tiff",
            "gif": "com.compuserve.gif", "heic": "public.heic", "pdf": "com.adobe.pdf", "txt": "public.plain-text",
            "json": "public.json", "xml": "public.xml", "html": "public.html", "htm": "public.html",
            "mp4": "public.mpeg-4", "m4v": "com.apple.m4v-video", "mov": "com.apple.quicktime-movie",
            "mkv": "org.matroska.mkv", "webm": "org.webmproject.webm", "avi": "public.avi",
            "mp3": "public.mp3", "m4a": "com.apple.m4a-audio", "wav": "com.microsoft.waveform-audio",
            "aiff": "public.aiff-audio", "aif": "public.aiff-audio", "flac": "org.xiph.flac", "ogg": "org.xiph.ogg",
            "zip": "public.zip-archive", "app": "com.apple.application-bundle",
        ]
        guard let identifier = known[filenameExtension.lowercased()] else {
            fatalError("UniformTypeIdentifiers: UTType(filenameExtension:conformingTo:) for .\(filenameExtension) "
                       + "is not implemented in Alchemy yet")
        }
        self.identifier = identifier
    }

    public var description: String { identifier }

    /// Whether the type conforms to another: needs the system's type database.
    public func conforms(to type: UTType) -> Bool {
        if type == self { return true }
        fatalError("UniformTypeIdentifiers: UTType.conforms(to:) is not implemented in Alchemy yet")
    }

    /// The type's preferred filename extension: needs the system's type database.
    public var preferredFilenameExtension: String? {
        fatalError("UniformTypeIdentifiers: UTType.preferredFilenameExtension is not implemented in Alchemy yet")
    }

    public static var item: UTType { UTType(identifier: "public.item") }
    public static var content: UTType { UTType(identifier: "public.content") }
    public static var data: UTType { UTType(identifier: "public.data") }
    public static var text: UTType { UTType(identifier: "public.text") }
    public static var plainText: UTType { UTType(identifier: "public.plain-text") }
    public static var utf8PlainText: UTType { UTType(identifier: "public.utf8-plain-text") }
    public static var url: UTType { UTType(identifier: "public.url") }
    public static var fileURL: UTType { UTType(identifier: "public.file-url") }
    public static var json: UTType { UTType(identifier: "public.json") }
    public static var image: UTType { UTType(identifier: "public.image") }
    public static var png: UTType { UTType(identifier: "public.png") }
    public static var jpeg: UTType { UTType(identifier: "public.jpeg") }
    public static var tiff: UTType { UTType(identifier: "public.tiff") }
    public static var pdf: UTType { UTType(identifier: "com.adobe.pdf") }
    public static var folder: UTType { UTType(identifier: "public.folder") }
    public static var directory: UTType { UTType(identifier: "public.directory") }
    public static var application: UTType { UTType(identifier: "com.apple.application") }
    public static var applicationBundle: UTType { UTType(identifier: "com.apple.application-bundle") }
}
