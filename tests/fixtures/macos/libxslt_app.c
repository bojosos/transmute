/* Project-authored fixture: an XSLT transform through the guest libxslt and
 * libexslt runtimes (/usr/lib/libxslt.1.dylib and /usr/lib/libexslt.0.dylib built
 * by scripts/build_alchemy_libxslt.ps1) on top of the guest libxml2. Covers a
 * stylesheet with keys, sorting, xsl:number and EXSLT string, math and set
 * functions, output to a string, and a stylesheet error.
 * Compiled against the public headers of those runtimes and runtime/include. */
#include <libexslt/exslt.h>
#include <libxml/parser.h>
#include <libxslt/transform.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/xsltutils.h>
#include <stdio.h>
#include <string.h>

static int stylesheet_errors;
static void on_error(void* context, const char* format, ...) { (void)context; (void)format; stylesheet_errors++; }

int main(void) {
    xmlInitParser();
    exsltRegisterAll();
    const char* data =
        "<orders>"
        "<order id='3' customer='kim'><item price='2.5'/><item price='4'/></order>"
        "<order id='1' customer='ana'><item price='10'/></order>"
        "<order id='2' customer='kim'><item price='1.25'/></order>"
        "</orders>";
    const char* sheet =
        "<xsl:stylesheet version='1.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'"
        " xmlns:str='http://exslt.org/strings' xmlns:math='http://exslt.org/math'"
        " xmlns:set='http://exslt.org/sets' extension-element-prefixes='str math set'>"
        "<xsl:output method='text'/>"
        "<xsl:key name='by-customer' match='order' use='@customer'/>"
        "<xsl:template match='/'>"
        "<xsl:for-each select='orders/order'><xsl:sort select='@id' data-type='number'/>"
        "<xsl:number value='position()' format='i'/>. order <xsl:value-of select='@id'/> "
        "<xsl:value-of select='@customer'/> total <xsl:value-of select='sum(item/@price)'/>&#10;</xsl:for-each>"
        "kim has <xsl:value-of select='count(key(\"by-customer\",\"kim\"))'/> orders&#10;"
        "max item <xsl:value-of select='math:max(//item/@price)'/>&#10;"
        "customers <xsl:value-of select='count(set:distinct(//order/@customer))'/>&#10;"
        "tokens <xsl:for-each select='str:tokenize(\"a,b;c\", \",;\")'><xsl:value-of select='.'/>|</xsl:for-each>&#10;"
        "padded [<xsl:value-of select='str:padding(3, \"-\")'/>]&#10;"
        "</xsl:template></xsl:stylesheet>";
    xmlDocPtr style_doc = xmlReadMemory(sheet, (int)strlen(sheet), "sheet.xsl", NULL, 0);
    xsltStylesheetPtr style = xsltParseStylesheetDoc(style_doc);
    xmlDocPtr input = xmlReadMemory(data, (int)strlen(data), "orders.xml", NULL, 0);
    xmlDocPtr result = xsltApplyStylesheet(style, input, NULL);
    xmlChar* text = NULL;
    int length = 0;
    xsltSaveResultToString(&text, &length, result, style);
    printf("%s", text ? (const char*)text : "(no result)\n");
    xmlFree(text);
    xmlFreeDoc(result);
    xmlFreeDoc(input);
    xsltFreeStylesheet(style);

    /* An unknown XSLT element is a stylesheet compile error. */
    xsltSetGenericErrorFunc(NULL, on_error);
    const char* broken = "<xsl:stylesheet version='1.0' xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>"
                         "<xsl:template match='/'><xsl:bogus/></xsl:template></xsl:stylesheet>";
    xmlDocPtr broken_doc = xmlReadMemory(broken, (int)strlen(broken), "broken.xsl", NULL, 0);
    xsltStylesheetPtr broken_style = xsltParseStylesheetDoc(broken_doc);
    printf("broken stylesheet: %s, errors %s\n", broken_style ? "compiled" : "rejected", stylesheet_errors ? "reported" : "none");
    if (broken_style) xsltFreeStylesheet(broken_style); else xmlFreeDoc(broken_doc);
    xsltCleanupGlobals();
    xmlCleanupParser();
    return 0;
}
