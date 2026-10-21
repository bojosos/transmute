// qsort/bsearch/qsort_s/_lfind with guest callbacks invoked by the CRT.
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long compare_calls;

static int cmp_int(const void* a, const void* b) {
    compare_calls++;
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static int cmp_str(const void* a, const void* b) {
    compare_calls++;
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

typedef struct {
    int group;
    double score;
    char name[12];
} Record;

static int cmp_record(const void* a, const void* b) {
    const Record* x = (const Record*)a;
    const Record* y = (const Record*)b;
    if (x->group != y->group) return x->group - y->group;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    return strcmp(x->name, y->name);
}

static int cmp_ctx(void* context, const void* a, const void* b) {
    int direction = *(int*)context;
    int x = *(const int*)a, y = *(const int*)b;
    return direction * ((x > y) - (x < y));
}

static uint32_t state = 12345;
static uint32_t rnd(void) {
    state = state * 1103515245u + 12345u;
    return (state >> 8) & 0xffffff;
}

int main(void) {
    enum { N = 50000 };
    static int values[N];
    for (int i = 0; i < N; i++) values[i] = (int)(rnd() % 100000) - 50000;
    qsort(values, N, sizeof values[0], cmp_int);
    int sorted = 1;
    unsigned long long hash = 0;
    for (int i = 0; i < N; i++) {
        if (i && values[i - 1] > values[i]) sorted = 0;
        hash = hash * 1000003u + (unsigned)values[i];
    }
    printf("qsort ints sorted=%d first=%d last=%d hash=%016llx calls=%llu\n", sorted, values[0], values[N - 1], hash,
           compare_calls);

    int hits = 0, misses = 0;
    for (int key = -50000; key < 50000; key += 37) {
        int* found = (int*)bsearch(&key, values, N, sizeof values[0], cmp_int);
        if (found) {
            hits++;
            if (*found != key) misses += 1000;
        } else {
            misses++;
        }
    }
    printf("bsearch hits=%d misses=%d\n", hits, misses);

    const char* words[] = {"pear", "apple", "fig", "banana", "cherry", "date", "elderberry", "grape", "kiwi", "lemon",
                           "mango", "apricot", "Zebra", "zebra", "", "apple"};
    size_t nwords = sizeof words / sizeof words[0];
    compare_calls = 0;
    qsort(words, nwords, sizeof words[0], cmp_str);
    printf("qsort strings:");
    for (size_t i = 0; i < nwords; i++) printf(" [%s]", words[i]);
    printf("\n");
    const char* key = "kiwi";
    const char** w = (const char**)bsearch(&key, words, nwords, sizeof words[0], cmp_str);
    printf("bsearch kiwi index=%d\n", w ? (int)(w - words) : -1);

    Record records[200];
    for (int i = 0; i < 200; i++) {
        records[i].group = (int)(rnd() % 5);
        records[i].score = (double)(rnd() % 50) / 4.0;
        snprintf(records[i].name, sizeof records[i].name, "r%03d", (int)(rnd() % 1000));
    }
    qsort(records, 200, sizeof records[0], cmp_record);
    printf("records:");
    for (int i = 0; i < 200; i += 23) printf(" %d/%.2f/%s", records[i].group, records[i].score, records[i].name);
    printf("\n");

    int desc[64];
    for (int i = 0; i < 64; i++) desc[i] = (int)(rnd() % 1000);
    int direction = -1;
    qsort_s(desc, 64, sizeof desc[0], cmp_ctx, &direction);
    printf("qsort_s desc:");
    for (int i = 0; i < 64; i += 8) printf(" %d", desc[i]);
    printf("\n");

    unsigned count = 64;
    int needle = desc[40];
    int* lf = (int*)_lfind(&needle, desc, &count, sizeof desc[0], cmp_int);
    printf("_lfind found=%d value-match=%d\n", lf != NULL, lf && *lf == needle);
    int absent = 5000;
    lf = (int*)_lfind(&absent, desc, &count, sizeof desc[0], cmp_int);
    printf("_lfind absent=%d\n", lf == NULL);
    fflush(stdout);
    return 0;
}
