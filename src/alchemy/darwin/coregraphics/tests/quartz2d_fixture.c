/* Quartz 2D guest fixture (owner: Q): draws into a CGBitmapContext with
 * CoreGraphics and CoreText, round-trips the result through ImageIO (PNG)
 * and prints pixel probes plus an FNV-1a checksum of the final pixels.
 * Project-authored declarations only (no Apple headers). */
typedef unsigned long size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef long CFIndex;
typedef double CGFloat;
typedef const void *CFTypeRef;
typedef struct CGPoint { CGFloat x, y; } CGPoint;
typedef struct CGSize { CGFloat width, height; } CGSize;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
typedef struct CGAffineTransform { CGFloat a, b, c, d, tx, ty; } CGAffineTransform;
typedef void *Ref;

extern int printf(const char *format, ...);

extern const char kCFTypeDictionaryKeyCallBacks[], kCFTypeDictionaryValueCallBacks[];
extern Ref CFStringCreateWithCString(Ref allocator, const char *text, uint32_t encoding);
extern Ref CFDictionaryCreate(Ref allocator, const void **keys, const void **values, CFIndex count, const void *key_callbacks,
                              const void *value_callbacks);
extern Ref CFDataCreateMutable(Ref allocator, CFIndex capacity);
extern CFIndex CFDataGetLength(Ref data);
extern void CFRelease(CFTypeRef object);
extern Ref CFAttributedStringCreate(Ref allocator, Ref string, Ref attributes);

extern Ref CGColorSpaceCreateDeviceRGB(void);
extern void CGColorSpaceRelease(Ref space);
extern Ref CGColorCreateGenericRGB(CGFloat r, CGFloat g, CGFloat b, CGFloat a);
extern void CGColorRelease(Ref color);
extern Ref CGBitmapContextCreate(void *data, size_t width, size_t height, size_t bits_per_component, size_t bytes_per_row, Ref space,
                                 uint32_t info);
extern void *CGBitmapContextGetData(Ref context);
extern size_t CGBitmapContextGetBytesPerRow(Ref context);
extern Ref CGBitmapContextCreateImage(Ref context);
extern void CGContextRelease(Ref context);
extern void CGContextSaveGState(Ref context);
extern void CGContextRestoreGState(Ref context);
extern void CGContextTranslateCTM(Ref context, CGFloat tx, CGFloat ty);
extern void CGContextRotateCTM(Ref context, CGFloat angle);
extern void CGContextSetRGBFillColor(Ref context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
extern void CGContextSetRGBStrokeColor(Ref context, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
extern void CGContextSetLineWidth(Ref context, CGFloat width);
extern void CGContextFillRect(Ref context, CGRect rect);
extern void CGContextStrokeRect(Ref context, CGRect rect);
extern void CGContextFillEllipseInRect(Ref context, CGRect rect);
extern void CGContextClipToRect(Ref context, CGRect rect);
extern void CGContextAddPath(Ref context, Ref path);
extern void CGContextEOFillPath(Ref context);
extern void CGContextDrawLinearGradient(Ref context, Ref gradient, CGPoint start, CGPoint end, uint32_t options);
extern void CGContextDrawImage(Ref context, CGRect rect, Ref image);
extern void CGContextSetTextPosition(Ref context, CGFloat x, CGFloat y);
extern Ref CGPathCreateMutable(void);
extern void CGPathAddRect(Ref path, const CGAffineTransform *transform, CGRect rect);
extern void CGPathRelease(Ref path);
extern Ref CGGradientCreateWithColorComponents(Ref space, const CGFloat *components, const CGFloat *locations, size_t count);
extern void CGGradientRelease(Ref gradient);
extern size_t CGImageGetWidth(Ref image);
extern size_t CGImageGetHeight(Ref image);
extern void CGImageRelease(Ref image);

extern const Ref kCTFontAttributeName, kCTForegroundColorAttributeName;
extern Ref CTFontCreateWithName(Ref name, CGFloat size, const CGAffineTransform *matrix);
extern CGFloat CTFontGetAscent(Ref font);
extern CGFloat CTFontGetDescent(Ref font);
extern uint32_t CTFontGetUnitsPerEm(Ref font);
extern Ref CTLineCreateWithAttributedString(Ref string);
extern double CTLineGetTypographicBounds(Ref line, CGFloat *ascent, CGFloat *descent, CGFloat *leading);
extern CFIndex CTLineGetGlyphCount(Ref line);
extern void CTLineDraw(Ref line, Ref context);

extern Ref CGImageDestinationCreateWithData(Ref data, Ref type, size_t count, Ref options);
extern void CGImageDestinationAddImage(Ref destination, Ref image, Ref properties);
extern _Bool CGImageDestinationFinalize(Ref destination);
extern Ref CGImageSourceCreateWithData(Ref data, Ref options);
extern size_t CGImageSourceGetCount(Ref source);
extern Ref CGImageSourceCreateImageAtIndex(Ref source, size_t index, Ref options);

enum { width = 96, height = 64, utf8 = 0x08000100, premultiplied_last = 1 };

static CGRect rect(CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
    CGRect r = {{x, y}, {w, h}};
    return r;
}
static Ref string(const char *text) { return CFStringCreateWithCString(0, text, utf8); }

/* Pixel in CoreGraphics coordinates (y up) of an RGBA8 premultiplied context. */
static const uint8_t *pixel(Ref context, int x, int y) {
    const uint8_t *data = (const uint8_t *)CGBitmapContextGetData(context);
    return data + (size_t)(height - 1 - y) * CGBitmapContextGetBytesPerRow(context) + (size_t)x * 4;
}
static int probe(const char *what, Ref context, int x, int y, int r, int g, int b, int a) {
    const uint8_t *p = pixel(context, x, y);
    int ok = 1;
    const int expected[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i) {
        const int delta = (int)p[i] - expected[i];
        if (delta > 3 || delta < -3) ok = 0;
    }
    printf("quartz2d_fixture: %s (%d,%d) = %d,%d,%d,%d%s\n", what, x, y, p[0], p[1], p[2], p[3], ok ? "" : " MISMATCH");
    return ok;
}

int main(void) {
    printf("quartz2d_fixture: start\n");
    Ref space = CGColorSpaceCreateDeviceRGB();
    Ref context = CGBitmapContextCreate(0, width, height, 8, 0, space, premultiplied_last);
    if (!context || !CGBitmapContextGetData(context)) {
        printf("quartz2d_fixture: CGBitmapContextCreate failed\n");
        return 1;
    }

    /* Shapes: a gradient background, a clipped rectangle, an ellipse, an
     * even-odd ring and a stroked frame under a translated CTM. */
    const CGFloat components[8] = {0, 0, 0.5, 1, 0, 0.5, 1, 1};
    Ref gradient = CGGradientCreateWithColorComponents(space, components, 0, 2);
    CGPoint start = {0, 0}, end = {width, 0};
    CGContextDrawLinearGradient(context, gradient, start, end, 3);
    CGGradientRelease(gradient);

    CGContextSaveGState(context);
    CGContextClipToRect(context, rect(4, 4, 20, 20));
    CGContextSetRGBFillColor(context, 1, 0, 0, 1);
    CGContextFillRect(context, rect(0, 0, 40, 40));
    CGContextRestoreGState(context);

    CGContextSetRGBFillColor(context, 0, 1, 0, 1);
    CGContextFillEllipseInRect(context, rect(28, 4, 20, 20));

    Ref ring = CGPathCreateMutable();
    CGPathAddRect(ring, 0, rect(52, 4, 20, 20));
    CGPathAddRect(ring, 0, rect(58, 10, 8, 8));
    CGContextAddPath(context, ring);
    CGContextSetRGBFillColor(context, 1, 1, 0, 1);
    CGContextEOFillPath(context);
    CGPathRelease(ring);

    CGContextSaveGState(context);
    CGContextTranslateCTM(context, 76, 4);
    CGContextSetRGBStrokeColor(context, 1, 1, 1, 1);
    CGContextSetLineWidth(context, 2);
    CGContextStrokeRect(context, rect(1, 1, 14, 14));
    CGContextRestoreGState(context);

    int ok = 1;
    ok &= probe("clipped rect", context, 10, 10, 255, 0, 0, 255);
    ok &= pixel(context, 26, 10)[0] < 40;   /* outside the clip: gradient, not red */
    ok &= probe("ellipse centre", context, 38, 14, 0, 255, 0, 255);
    ok &= probe("ring", context, 54, 14, 255, 255, 0, 255);
    ok &= pixel(context, 62, 14)[1] < 200;   /* the ring's hole shows the gradient */
    ok &= probe("stroked frame", context, 77, 10, 255, 255, 255, 255);
    printf("quartz2d_fixture: shapes %s\n", ok ? "ok" : "FAILED");
    if (!ok) return 1;

    /* Text: CoreText line in Helvetica (mapped to a Windows face). */
    Ref font = CTFontCreateWithName(string("Helvetica"), 18, 0);
    const CGFloat ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font);
    Ref white = CGColorCreateGenericRGB(1, 1, 1, 1);
    const void *keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void *values[2] = {font, white};
    Ref attributes = CFDictionaryCreate(0, keys, values, 2, kCFTypeDictionaryKeyCallBacks, kCFTypeDictionaryValueCallBacks);
    Ref text = CFAttributedStringCreate(0, string("Quartz"), attributes);
    Ref line = CTLineCreateWithAttributedString(text);
    CGFloat line_ascent = 0, line_descent = 0, leading = 0;
    const double line_width = CTLineGetTypographicBounds(line, &line_ascent, &line_descent, &leading);
    CGContextSetTextPosition(context, 4, 36);
    CTLineDraw(line, context);
    int lit = 0, min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int y = 28; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint8_t *p = pixel(context, x, y);
            if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
                ++lit;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    printf("quartz2d_fixture: font ascent=%d descent=%d upem=%u glyphs=%ld width=%d\n", (int)(ascent * 100), (int)(descent * 100),
           CTFontGetUnitsPerEm(font), (long)CTLineGetGlyphCount(line), (int)(line_width * 100));
    printf("quartz2d_fixture: text pixels=%d bounds=%d,%d-%d,%d\n", lit, min_x, min_y, max_x, max_y);
    const int text_ok = CTLineGetGlyphCount(line) == 6 && line_width > 30 && line_width < 90 && lit > 40 && min_x >= 3 &&
                        max_x <= 4 + (int)line_width + 1 && max_y <= 36 + (int)line_ascent + 1 && min_y >= 36 - (int)line_descent - 2 &&
                        line_ascent == ascent && line_descent == descent;
    printf("quartz2d_fixture: text %s\n", text_ok ? "ok" : "FAILED");
    CFRelease(line);
    CFRelease(text);
    CFRelease(attributes);
    CGColorRelease(white);
    CFRelease(font);
    if (!text_ok) return 1;

    /* ImageIO: encode as PNG, decode, draw the decoded image into a second
     * context and compare every pixel. */
    Ref image = CGBitmapContextCreateImage(context);
    Ref png = CFDataCreateMutable(0, 0);
    Ref destination = CGImageDestinationCreateWithData(png, string("public.png"), 1, 0);
    CGImageDestinationAddImage(destination, image, 0);
    const int encoded = CGImageDestinationFinalize(destination) && CFDataGetLength(png) > 100;
    Ref source = CGImageSourceCreateWithData(png, 0);
    Ref decoded = source && CGImageSourceGetCount(source) == 1 ? CGImageSourceCreateImageAtIndex(source, 0, 0) : 0;
    Ref copy = CGBitmapContextCreate(0, width, height, 8, 0, space, premultiplied_last);
    int differences = -1;
    if (decoded && CGImageGetWidth(decoded) == width && CGImageGetHeight(decoded) == height) {
        CGContextDrawImage(copy, rect(0, 0, width, height), decoded);
        differences = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint8_t *a = pixel(context, x, y), *b = pixel(copy, x, y);
                for (int i = 0; i < 4; ++i) {
                    const int delta = (int)a[i] - (int)b[i];
                    if (delta > 1 || delta < -1) ++differences;
                }
            }
    }
    printf("quartz2d_fixture: png bytes=%ld differences=%d\n", (long)CFDataGetLength(png), differences);
    const int io_ok = encoded && differences == 0;
    printf("quartz2d_fixture: imageio %s\n", io_ok ? "ok" : "FAILED");
    if (!io_ok) return 1;

    uint32_t hash = 2166136261u;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint8_t *p = pixel(context, x, y);
            for (int i = 0; i < 4; ++i) hash = (hash ^ p[i]) * 16777619u;
        }
    printf("quartz2d_fixture: checksum=%08x\n", hash);

    if (decoded) CGImageRelease(decoded);
    CFRelease(source);
    CFRelease(destination);
    CFRelease(png);
    CGImageRelease(image);
    CGContextRelease(copy);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    printf("quartz2d_fixture: done\n");
    return 0;
}
