/* Project-authored fixture: parses XML through the guest libexpat runtime
 * (/usr/lib/libexpat.1.dylib built by scripts/build_alchemy_expat.ps1), with
 * guest callbacks invoked by guest expat code. Covers every libexpat import of
 * a Blender add-on installer plus namespaces, chunked input, a custom
 * single-byte encoding, error reporting and dlopen of the runtime identity.
 * The expat declarations below are written from expat's documented API. */
typedef unsigned long size_t;
typedef char XML_Char;
typedef unsigned char XML_Bool;
typedef struct XML_ParserStruct* XML_Parser;
enum XML_Status { XML_STATUS_ERROR = 0, XML_STATUS_OK = 1, XML_STATUS_SUSPENDED = 2 };
typedef struct {
    int map[256];
    void* data;
    int (*convert)(void* data, const char* s);
    void (*release)(void* data);
} XML_Encoding;
typedef struct { int major, minor, micro; } XML_Expat_Version;

extern XML_Parser XML_ParserCreate(const XML_Char* encoding);
extern XML_Parser XML_ParserCreateNS(const XML_Char* encoding, XML_Char separator);
extern void XML_ParserFree(XML_Parser parser);
extern void XML_SetUserData(XML_Parser parser, void* data);
extern void XML_SetElementHandler(XML_Parser, void (*)(void*, const XML_Char*, const XML_Char**), void (*)(void*, const XML_Char*));
extern void XML_SetCharacterDataHandler(XML_Parser, void (*)(void*, const XML_Char*, int));
extern void XML_SetCommentHandler(XML_Parser, void (*)(void*, const XML_Char*));
extern void XML_SetProcessingInstructionHandler(XML_Parser, void (*)(void*, const XML_Char*, const XML_Char*));
extern void XML_SetCdataSectionHandler(XML_Parser, void (*)(void*), void (*)(void*));
extern void XML_SetDoctypeDeclHandler(XML_Parser, void (*)(void*, const XML_Char*, const XML_Char*, const XML_Char*, int), void (*)(void*));
extern void XML_SetDefaultHandler(XML_Parser, void (*)(void*, const XML_Char*, int));
extern void XML_SetUnknownEncodingHandler(XML_Parser, int (*)(void*, const XML_Char*, XML_Encoding*), void*);
extern enum XML_Status XML_Parse(XML_Parser parser, const char* s, int len, int isFinal);
extern int XML_GetErrorCode(XML_Parser parser);
extern const XML_Char* XML_ErrorString(int code);
extern unsigned long XML_GetCurrentLineNumber(XML_Parser parser);
extern const XML_Char* XML_ExpatVersion(void);
extern XML_Expat_Version XML_ExpatVersionInfo(void);
extern int printf(const char*, ...);
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);

struct State {
    int elements, depth, max_depth, attributes, comments, instructions, cdata_open, cdata_close;
    int doctype_open, doctype_close, default_bytes, encodings, text_length;
    unsigned long long checksum;
    char text[256];
    char doctype[32];
    char last_element[64];
};

static int length(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static int same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void copy(char* target, const char* source, int capacity) {
    int index = 0;
    for (; source[index] && index + 1 < capacity; ++index) target[index] = source[index];
    target[index] = 0;
}
static void mix(struct State* state, const char* s, int n) {
    for (int index = 0; index < n; ++index) state->checksum = state->checksum * 131 + (unsigned char)s[index];
}

static void start(void* data, const XML_Char* name, const XML_Char** attributes) {
    struct State* state = data;
    ++state->elements;
    if (++state->depth > state->max_depth) state->max_depth = state->depth;
    mix(state, name, length(name));
    for (int index = 0; attributes[index]; index += 2) {
        ++state->attributes;
        mix(state, attributes[index], length(attributes[index]));
        mix(state, attributes[index + 1], length(attributes[index + 1]));
    }
    copy(state->last_element, name, (int)sizeof(state->last_element));
}
static void end(void* data, const XML_Char* name) {
    struct State* state = data;
    --state->depth;
    mix(state, name, length(name));
}
static void characters(void* data, const XML_Char* s, int n) {
    struct State* state = data;
    for (int index = 0; index < n && state->text_length + 1 < (int)sizeof(state->text); ++index) {
        if (s[index] != '\n' && s[index] != ' ') state->text[state->text_length++] = s[index];
    }
    state->text[state->text_length] = 0;
    mix(state, s, n);
}
static void comment(void* data, const XML_Char* text) {
    struct State* state = data;
    ++state->comments;
    mix(state, text, length(text));
}
static void instruction(void* data, const XML_Char* target, const XML_Char* body) {
    struct State* state = data;
    ++state->instructions;
    mix(state, target, length(target));
    mix(state, body, length(body));
}
static void cdata_start(void* data) { ++((struct State*)data)->cdata_open; }
static void cdata_end(void* data) { ++((struct State*)data)->cdata_close; }
static void doctype_start(void* data, const XML_Char* name, const XML_Char* sys, const XML_Char* pub, int internal) {
    struct State* state = data;
    (void)sys; (void)pub;
    ++state->doctype_open;
    copy(state->doctype, name, (int)sizeof(state->doctype));
    state->checksum += (unsigned long long)internal;
}
static void doctype_end(void* data) { ++((struct State*)data)->doctype_close; }
static void fallback(void* data, const XML_Char* s, int n) {
    struct State* state = data;
    state->default_bytes += n;
    mix(state, s, n);
}
/* "x-alchemy-cyrillic": ASCII plus 0x80..0xFF mapped to U+0400..U+047F. */
static int encoding(void* data, const XML_Char* name, XML_Encoding* info) {
    struct State* state = data;
    if (!same(name, "x-alchemy-cyrillic")) return XML_STATUS_ERROR;
    ++state->encodings;
    for (int index = 0; index < 256; ++index) info->map[index] = index < 0x80 ? index : 0x400 + (index - 0x80);
    info->data = 0;
    info->convert = 0;
    info->release = 0;
    return XML_STATUS_OK;
}

static XML_Parser create(struct State* state, int namespaces) {
    XML_Parser parser = namespaces ? XML_ParserCreateNS(0, '|') : XML_ParserCreate(0);
    if (!parser) return 0;
    XML_SetUserData(parser, state);
    XML_SetElementHandler(parser, start, end);
    XML_SetCharacterDataHandler(parser, characters);
    XML_SetCommentHandler(parser, comment);
    XML_SetProcessingInstructionHandler(parser, instruction);
    XML_SetCdataSectionHandler(parser, cdata_start, cdata_end);
    XML_SetDoctypeDeclHandler(parser, doctype_start, doctype_end);
    XML_SetDefaultHandler(parser, fallback);
    XML_SetUnknownEncodingHandler(parser, encoding, state);
    return parser;
}

static const char document[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE installer [ <!ENTITY product \"Suite\"> ]>\n"
    "<installer version=\"7.40\" mode=\"headless\">\n"
    "  <!-- components -->\n"
    "  <?alchemy check=\"1\"?>\n"
    "  <component name=\"blender\" target=\"4.5\">&product; f\xC3\xBCr Blender</component>\n"
    "  <license><![CDATA[a<b&c]]></license>\n"
    "  <path>caf\xC3\xA9 &#x263A; &amp; &lt;done&gt;</path>\n"
    "</installer>\n";

/* Zero-initialized storage; the fixture links no memset. */
static struct State state, names, custom, broken;

int main(void) {
    XML_Expat_Version version = XML_ExpatVersionInfo();
    if (!same(XML_ExpatVersion(), "expat_2.8.5") || version.major != 2 || version.minor != 8) return 1;

    /* Chunked input: split inside a tag, an entity and a UTF-8 sequence. */
    XML_Parser parser = create(&state, 0);
    if (!parser) return 2;
    const int total = length(document);
    const int cuts[] = {7, 61, 150, 201, 202, total};
    int offset = 0;
    for (int index = 0; index < (int)(sizeof(cuts) / sizeof(cuts[0])); ++index) {
        const int final = cuts[index] == total;
        if (XML_Parse(parser, document + offset, cuts[index] - offset, final) != XML_STATUS_OK) {
            printf("parse error %d: %s at line %lu\n", XML_GetErrorCode(parser),
                   XML_ErrorString(XML_GetErrorCode(parser)), XML_GetCurrentLineNumber(parser));
            return 3;
        }
        offset = cuts[index];
    }
    const unsigned long lines = XML_GetCurrentLineNumber(parser);
    XML_ParserFree(parser);
    if (state.depth != 0 || state.cdata_open != 1 || state.cdata_close != 1 || state.doctype_close != 1) return 4;

    /* Namespaces with '|' separator. */
    parser = create(&names, 1);
    static const char spaced[] = "<a:r xmlns:a=\"urn:alchemy\" a:k=\"v\"><a:c/></a:r>";
    if (!parser || XML_Parse(parser, spaced, length(spaced), 1) != XML_STATUS_OK) return 5;
    XML_ParserFree(parser);
    if (!same(names.last_element, "urn:alchemy|c") || names.attributes != 1) return 6;

    /* Unknown single-byte encoding resolved by the guest handler. */
    parser = create(&custom, 0);
    static const char cyrillic[] = "<?xml version=\"1.0\" encoding=\"x-alchemy-cyrillic\"?><t>\xC1\xC2</t>";
    if (!parser || XML_Parse(parser, cyrillic, length(cyrillic), 1) != XML_STATUS_OK) return 7;
    XML_ParserFree(parser);
    if (custom.encodings != 1 || !same(custom.text, "\xD1\x81\xD1\x82")) return 8;

    /* Errors are reported with expat's code, message and line. */
    parser = create(&broken, 0);
    static const char invalid[] = "<a>\n<b>\n</a>";
    if (!parser || XML_Parse(parser, invalid, length(invalid), 1) != XML_STATUS_ERROR) return 9;
    const int error = XML_GetErrorCode(parser);
    const unsigned long error_line = XML_GetCurrentLineNumber(parser);
    XML_ParserFree(parser);

    /* The runtime identity is also reachable through dlopen. */
    void* library = dlopen("/usr/lib/libexpat.1.dylib", 2);
    const XML_Char* (*dynamic_version)(void) = library ? (const XML_Char* (*)(void))dlsym(library, "XML_ExpatVersion") : 0;
    if (!dynamic_version || dynamic_version != XML_ExpatVersion) return 10;

    printf("Expat %d.%d.%d: elements %d depth %d attributes %d comments %d pi %d cdata %d doctype %s default %d lines %lu text %s checksum %llu\n",
           version.major, version.minor, version.micro, state.elements, state.max_depth, state.attributes,
           state.comments, state.instructions, state.cdata_open, state.doctype, state.default_bytes, lines,
           state.text, state.checksum % 1000000007ULL);
    printf("Expat error %d (%s) line %lu; namespace %s; encoding %s\n", error, XML_ErrorString(error), error_line,
           names.last_element, custom.text);
    return 0;
}
