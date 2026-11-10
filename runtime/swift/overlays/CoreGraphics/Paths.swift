// The Swift CoreGraphics overlay's additions to CGContext, CGPath and CGRect, and CGPathFillRule (written for Alchemy
// from Apple's public documentation). CGPath's set operations need CoreGraphics' path boolean operations, which
// Alchemy does not provide yet; they stop explicitly.

/// The rule that decides which points a path's fill covers.
public enum CGPathFillRule: Int {
    case winding
    case evenOdd
}

extension CGContext {
    /// Draws the image in the rectangle, tiled across the context's clip when byTiling is true.
    public func draw(_ image: CGImage, in rect: CGRect, byTiling: Bool = false) {
        if byTiling { CGContextDrawTiledImage(self, rect, image) } else { CGContextDrawImage(self, rect, image) }
    }
}

extension CGPath {
    /// The union of this path and another.
    public func union(_ other: CGPath, using rule: CGPathFillRule = .winding) -> CGPath {
        fatalError("CoreGraphics: CGPath.union(_:using:) is not implemented in Alchemy yet")
    }
}

extension CGRect {
    /// The rectangle a dictionary made by CGRect.dictionaryRepresentation describes, or nil.
    public init?(dictionaryRepresentation dict: CFDictionary) {
        var rect = CGRect()
        guard CGRectMakeWithDictionaryRepresentation(dict, &rect) else { return nil }
        self = rect
    }
}
