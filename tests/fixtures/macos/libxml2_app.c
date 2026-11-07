/* Project-authored fixture: drives the guest libxml2 runtime (/usr/lib/libxml2.2.dylib
 * built by scripts/build_alchemy_libxml2.ps1). Covers the tree calls lld binds (read from
 * memory, build a document with a namespace, dump it formatted), XPath, the HTML parser,
 * the reader and writer interfaces, a UTF-32BE document converted through Alchemy's
 * libiconv provider, and error reporting through the generic error function.
 * Compiled against libxml2's public headers (third_party/libxml2, MIT) and runtime/include. */
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include <stdio.h>
#include <string.h>

static int error_calls;
static char first_error[128];

static void on_error(void* context, const char* format, ...) {
    (void)context;
    if (error_calls++ == 0) {
        size_t length = 0;
        for (; format[length] && length + 1 < sizeof first_error; ++length) first_error[length] = format[length];
        first_error[length] = 0;
        while (length && (first_error[length - 1] == '\n' || first_error[length - 1] == ' ')) first_error[--length] = 0;
    }
}

static void xpath_string(xmlDocPtr doc, const char* expression, char* out, size_t size) {
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    xmlXPathObjectPtr result = xmlXPathEvalExpression((const xmlChar*)expression, context);
    xmlChar* text = xmlXPathCastToString(result);
    snprintf(out, size, "%s", (const char*)text);
    xmlFree(text);
    xmlXPathFreeObject(result);
    xmlXPathFreeContext(context);
}

int main(void) {
    xmlInitParser();
    const char* source =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE catalog [<!ENTITY who \"Alchemy\">]>\n"
        "<catalog xmlns:t=\"urn:toolchain\" owner=\"&who;\">\n"
        "  <item id=\"a\" n=\"2\">first</item>\n"
        "  <item id=\"b\" n=\"5\"><![CDATA[<second>]]></item>\n"
        "  <t:note>caf\xC3\xA9</t:note>\n"
        "</catalog>\n";
    xmlDocPtr doc = xmlReadMemory(source, (int)strlen(source), "catalog.xml", NULL, XML_PARSE_NOENT);
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlChar* owner = xmlGetProp(root, (const xmlChar*)"owner");
    char count[32], sum[32], second[64], note[64];
    xpath_string(doc, "count(//item)", count, sizeof count);
    xpath_string(doc, "sum(//item/@n)", sum, sizeof sum);
    xpath_string(doc, "string(//item[@id='b'])", second, sizeof second);
    xpath_string(doc, "string(/catalog/*[local-name()='note'])", note, sizeof note);
    printf("tree: root %s owner %s items %s sum %s second %s note %s\n", (const char*)root->name, (const char*)owner,
           count, sum, second, note);
    xmlFree(owner);
    xmlFreeDoc(doc);

    /* Build and serialize, as lld does for its output files. */
    xmlDocPtr built = xmlNewDoc((const xmlChar*)"1.0");
    xmlNodePtr top = xmlNewNode(NULL, (const xmlChar*)"plist");
    xmlNsPtr ns = xmlNewNs(top, (const xmlChar*)"urn:alchemy", (const xmlChar*)"a");
    xmlSetNs(top, ns);
    xmlNewProp(top, (const xmlChar*)"version", (const xmlChar*)"1.0");
    xmlDocSetRootElement(built, top);
    xmlNewTextChild(top, NULL, (const xmlChar*)"key", (const xmlChar*)"x<y");
    xmlChar* dumped = NULL;
    int dumped_size = 0;
    xmlDocDumpFormatMemoryEnc(built, &dumped, &dumped_size, "UTF-8", 1);
    printf("dump: %d bytes\n%s", dumped_size, (const char*)dumped);
    xmlFree(dumped);
    xmlFreeDoc(built);

    /* UTF-32BE goes through iconv (libxml2 has no built-in UTF-32 decoder). */
    const unsigned codepoints[] = {'<', 'v', '>', 0xE9, 0x263A, '<', '/', 'v', '>'};
    unsigned char wide[sizeof codepoints];
    for (size_t index = 0; index < sizeof codepoints / sizeof codepoints[0]; ++index) {
        wide[index * 4] = (unsigned char)(codepoints[index] >> 24);
        wide[index * 4 + 1] = (unsigned char)(codepoints[index] >> 16);
        wide[index * 4 + 2] = (unsigned char)(codepoints[index] >> 8);
        wide[index * 4 + 3] = (unsigned char)codepoints[index];
    }
    xmlDocPtr utf32 = xmlReadMemory((const char*)wide, (int)sizeof wide, "wide.xml", "UTF-32BE", 0);
    xmlChar* utf32_text = utf32 ? xmlNodeGetContent(xmlDocGetRootElement(utf32)) : NULL;
    printf("utf-32be: %s\n", utf32_text ? (const char*)utf32_text : "(not decoded)");
    xmlFree(utf32_text);
    xmlFreeDoc(utf32);

    /* HTML parser: recovers the unclosed tags. */
    const char* page = "<html><body><p>one<p>two<br>three</body></html>";
    htmlDocPtr html = htmlReadMemory(page, (int)strlen(page), "page.html", NULL, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    char paragraphs[32];
    xpath_string(html, "count(//p)", paragraphs, sizeof paragraphs);
    printf("html: paragraphs %s\n", paragraphs);
    xmlFreeDoc(html);

    /* Reader and writer. */
    const char* stream = "<r><a/><b>t</b><c x='1'/></r>";
    xmlTextReaderPtr reader = xmlReaderForMemory(stream, (int)strlen(stream), "stream.xml", NULL, 0);
    int elements = 0;
    while (xmlTextReaderRead(reader) == 1)
        if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) elements++;
    xmlFreeTextReader(reader);
    xmlBufferPtr buffer = xmlBufferCreate();
    xmlTextWriterPtr writer = xmlNewTextWriterMemory(buffer, 0);
    xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);
    xmlTextWriterStartElement(writer, (const xmlChar*)"w");
    xmlTextWriterWriteAttribute(writer, (const xmlChar*)"k", (const xmlChar*)"v&");
    xmlTextWriterWriteString(writer, (const xmlChar*)"body");
    xmlTextWriterEndDocument(writer);
    xmlFreeTextWriter(writer);
    printf("reader: elements %d; writer: %s", elements, (const char*)xmlBufferContent(buffer));
    xmlBufferFree(buffer);

    /* A malformed document reports through the generic error function. */
    xmlSetGenericErrorFunc(NULL, on_error);
    const char* broken = "<a><b></a>";
    xmlDocPtr bad = xmlReadMemory(broken, (int)strlen(broken), "broken.xml", NULL, 0);
    const xmlError* last = xmlGetLastError();
    printf("error: doc %s code %d calls %s\n", bad ? "parsed" : "null", last ? last->code : -1, error_calls > 0 ? "yes" : "no");
    xmlFreeDoc(bad);
    xmlCleanupParser();
    return 0;
}
