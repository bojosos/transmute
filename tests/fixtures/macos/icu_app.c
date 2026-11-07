/* Project-authored fixture: uses the guest ICU runtime (/usr/lib/libicucore.A.dylib built by
 * scripts/build_alchemy_icu.ps1) the way libpsl and raptor2 do: UTF-8 to UTF-16 and back, case
 * mapping (root and Turkish), and UTS #46 IDNA (non-transitional to ASCII and back, with an error
 * case), which needs the compiled-in uts46 normalization data. The ICU declarations below are
 * written from ICU's documented C API (unversioned names, as libicucore exports them). */
typedef unsigned short UChar;
typedef int UErrorCode;
typedef signed char UBool;
typedef struct UIDNA UIDNA;
typedef struct {
    short size;
    UBool isTransitionalDifferent;
    UBool reservedB3;
    unsigned errors;
    int reservedI2;
    int reservedI3;
} UIDNAInfo;
enum { U_ZERO_ERROR = 0, U_BUFFER_OVERFLOW_ERROR = 15 };
enum { UIDNA_USE_STD3_RULES = 2, UIDNA_CHECK_BIDI = 4, UIDNA_CHECK_CONTEXTJ = 8, UIDNA_NONTRANSITIONAL_TO_ASCII = 0x10,
       UIDNA_NONTRANSITIONAL_TO_UNICODE = 0x20 };
extern UChar* u_strFromUTF8(UChar* dest, int capacity, int* length, const char* src, int srcLength, UErrorCode* error);
extern char* u_strToUTF8(char* dest, int capacity, int* length, const UChar* src, int srcLength, UErrorCode* error);
extern int u_strToLower(UChar* dest, int capacity, const UChar* src, int srcLength, const char* locale, UErrorCode* error);
extern int u_strToUpper(UChar* dest, int capacity, const UChar* src, int srcLength, const char* locale, UErrorCode* error);
extern const char* u_errorName(UErrorCode code);
extern void u_getVersion(unsigned char version[4]);
extern UIDNA* uidna_openUTS46(unsigned options, UErrorCode* error);
extern void uidna_close(UIDNA* idna);
extern int uidna_nameToASCII_UTF8(const UIDNA* idna, const char* name, int length, char* dest, int capacity,
    UIDNAInfo* info, UErrorCode* error);
extern int uidna_nameToUnicodeUTF8(const UIDNA* idna, const char* name, int length, char* dest, int capacity,
    UIDNAInfo* info, UErrorCode* error);
extern int uidna_nameToASCII(const UIDNA* idna, const UChar* name, int length, UChar* dest, int capacity,
    UIDNAInfo* info, UErrorCode* error);
extern int printf(const char*, ...);

static const char* mapped(int upper, const char* text, const char* locale) {
    static char out[256];
    UChar wide[128], cased[128];
    UErrorCode error = U_ZERO_ERROR;
    int length = 0;
    u_strFromUTF8(wide, 128, &length, text, -1, &error);
    length = upper ? u_strToUpper(cased, 128, wide, length, locale, &error) : u_strToLower(cased, 128, wide, length, locale, &error);
    u_strToUTF8(out, 256, 0, cased, length, &error);
    return error == U_ZERO_ERROR ? out : u_errorName(error);
}

static void to_ascii(const UIDNA* idna, const char* name) {
    char out[256];
    UIDNAInfo info = { sizeof(UIDNAInfo), 0, 0, 0, 0, 0 };
    UErrorCode error = U_ZERO_ERROR;
    const int length = uidna_nameToASCII_UTF8(idna, name, -1, out, 255, &info, &error);
    out[length >= 0 && length < 256 ? length : 0] = 0;
    printf("toASCII %s -> %s errors 0x%x %s\n", name, out, info.errors, u_errorName(error));
}

int main(void) {
    unsigned char version[4];
    u_getVersion(version);
    printf("ICU %d.%d\n", version[0], version[1]);

    const char* text = "\xC4\xB0STANBUL Stra\xC3\x9F" "e \xCE\xA9MEGA";   /* İSTANBUL Straße ΩMEGA */
    printf("lower root: %s\n", mapped(0, text, ""));
    printf("lower tr: %s\n", mapped(0, text, "tr"));
    printf("upper: %s\n", mapped(1, text, "en"));

    UErrorCode error = U_ZERO_ERROR;
    UIDNA* idna = uidna_openUTS46(UIDNA_NONTRANSITIONAL_TO_ASCII | UIDNA_NONTRANSITIONAL_TO_UNICODE | UIDNA_CHECK_BIDI |
        UIDNA_CHECK_CONTEXTJ | UIDNA_USE_STD3_RULES, &error);
    if (!idna) { printf("uidna_openUTS46: %s\n", u_errorName(error)); return 1; }
    to_ascii(idna, "B\xC3\xBC" "cher.example");
    to_ascii(idna, "B\xC3\x9C" "CHER.EXAMPLE");
    to_ascii(idna, "fa\xC3\x9F.de");
    to_ascii(idna, "a..b");
    char back[256];
    UIDNAInfo info = { sizeof(UIDNAInfo), 0, 0, 0, 0, 0 };
    error = U_ZERO_ERROR;
    const int length = uidna_nameToUnicodeUTF8(idna, "xn--bcher-kva.example", -1, back, 255, &info, &error);
    back[length >= 0 && length < 256 ? length : 0] = 0;
    printf("toUnicode xn--bcher-kva.example -> %s errors 0x%x\n", back, info.errors);

    UChar small[4];
    error = U_ZERO_ERROR;
    const UChar wide_name[] = { 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0 };
    const int needed = uidna_nameToASCII(idna, wide_name, -1, small, 4, &info, &error);
    printf("preflight %d %s\n", needed, u_errorName(error));
    uidna_close(idna);
    return 0;
}
