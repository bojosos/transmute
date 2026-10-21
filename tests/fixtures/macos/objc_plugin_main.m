#include "objc_leaf.h"
@interface AlchemyLeaf (PluginAddition)
- (unsigned long)pluginValue;
@end
typedef void *pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
extern int pthread_mutex_lock(pthread_mutex_t *);
extern int pthread_mutex_unlock(pthread_mutex_t *);
extern int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
extern int pthread_cond_broadcast(pthread_cond_t *);
static pthread_mutex_t mutex = {0x32aaaba7, {0}};
static pthread_cond_t condition = {0x3cb0b1bb, {0}};
static int ready, released, destroyed;
static Class plugin_class;
@interface PluginPoolProbe : NSObject
@end
@implementation PluginPoolProbe
- (void)dealloc { __atomic_add_fetch(&destroyed, 1, __ATOMIC_SEQ_CST); [super dealloc]; }
@end
static void *worker(void *argument) {
    @autoreleasepool {
        [[[PluginPoolProbe alloc] init] autorelease];
        if (pthread_mutex_lock(&mutex)) return (void *)1;
        ++ready;
        if (pthread_cond_broadcast(&condition)) return (void *)2;
        while (!released) if (pthread_cond_wait(&condition, &mutex)) return (void *)3;
        Class cls = plugin_class;
        if (pthread_mutex_unlock(&mutex)) return (void *)4;
        if (!cls) return (void *)5;
        AlchemyLeaf *plugin = [[cls alloc] init];
        AlchemyLeaf *leaf = alchemy_leaf_create();
        int valid = [plugin value] == 42 && [leaf pluginValue] == 27;
        [plugin release]; [leaf release];
        if (!valid) return (void *)6;
    }
    return argument;
}
static int wake_and_join(pthread_t first, pthread_t second, int status) {
    void *a = 0, *b = 0;
    if (pthread_mutex_lock(&mutex)) return 20;
    released = 1;
    if (pthread_cond_broadcast(&condition) || pthread_mutex_unlock(&mutex)) return 21;
    if ((first && pthread_join(first, &a)) || (second && pthread_join(second, &b))) return 22;
    if (!status && (a != (void *)27 || b != (void *)42)) return 23;
    return status;
}
static int exercise(int argc, char **argv) {
    if (argc != 2) return 1;
    pthread_t first = 0, second = 0;
    if (pthread_create(&first, 0, worker, (void *)27)) return 11;
    if (pthread_create(&second, 0, worker, (void *)42)) return wake_and_join(first, 0, 12);
    if (pthread_mutex_lock(&mutex)) return wake_and_join(first, second, 13);
    while (ready != 2) if (pthread_cond_wait(&condition, &mutex)) return 14;
    if (pthread_mutex_unlock(&mutex)) return 15;
    AlchemyLeaf *before = alchemy_leaf_create();
    void *handle = dlopen(argv[1], 2);
    if (!handle) { [before release]; return wake_and_join(first, second, 2); }
    Class (*get_class)(void) = (Class (*)(void))dlsym(handle, "alchemy_plugin_class");
    int (*get_loads)(void) = (int (*)(void))dlsym(handle, "alchemy_plugin_loads");
    if (!get_class || !get_loads || get_loads() != 2) { [before release]; return wake_and_join(first, second, 3); }
    id plugin = [[get_class() alloc] init];
    if ([plugin value] != 42 || [before pluginValue] != 27) {
        [plugin release]; [before release]; return wake_and_join(first, second, 4);
    }
    if (dlclose(handle) != 0) { [plugin release]; [before release]; return wake_and_join(first, second, 5); }
    handle = dlopen(argv[1], 2);
    if (!handle || get_loads() != 2) { [plugin release]; [before release]; return wake_and_join(first, second, 6); }
    plugin_class = get_class();
    [plugin release]; [before release]; dlclose(handle);
    return wake_and_join(first, second, 0);
}
int main(int argc, char **argv) {
    int status;
    @autoreleasepool {
        [[[PluginPoolProbe alloc] init] autorelease];
        status = exercise(argc, argv);
        if (!status && __atomic_load_n(&destroyed, __ATOMIC_SEQ_CST) != 2) status = 7;
    }
    if (!status && destroyed != 3) status = 8;
    if (status) return status;
    puts("Objective-C plugin: 42 27");
    return 0;
}
