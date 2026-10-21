/* Declaration-only linking behavior; these bodies never run in Alchemy. */
typedef unsigned long dispatch_time_t;
void *dispatch_semaphore_create(long value) { (void)value; return 0; }
long dispatch_semaphore_wait(void *value, dispatch_time_t time) { (void)value; (void)time; return 0; }
long dispatch_semaphore_signal(void *value) { (void)value; return 0; }
void dispatch_retain(void *value) { (void)value; }
void dispatch_release(void *value) { (void)value; }
dispatch_time_t dispatch_time(dispatch_time_t time, long delta) { (void)delta; return time; }
dispatch_time_t dispatch_walltime(const void *time, long delta) { (void)time; (void)delta; return 0; }
