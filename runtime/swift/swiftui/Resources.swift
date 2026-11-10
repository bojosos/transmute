// SwiftUI for Alchemy: the declarations that name other frameworks' types, asset resources (DeveloperToolsSupport)
// and uniform types (UniformTypeIdentifiers) (written from Apple's public SwiftUI documentation; nothing here is Apple
// code). Color and Image are @frozen as Apple documents them, so their members stop until the layout survey
// establishes their layouts.

import Foundation
import DeveloperToolsSupport
import UniformTypeIdentifiers

extension Color {
    public init(_ resource: ColorResource) { _alchemyLayoutUnestablished("Color") }
}

extension Image {
    public init(_ resource: ImageResource) { _alchemyLayoutUnestablished("Image") }
}

extension View {
    public func fileImporter(isPresented: Binding<Bool>, allowedContentTypes: [UTType],
                             onCompletion: @escaping (Result<URL, Error>) -> Void) -> some View {
        let never: Never = _alchemyUnimplemented("View.fileImporter(isPresented:allowedContentTypes:onCompletion:)")
        return never
    }
}
