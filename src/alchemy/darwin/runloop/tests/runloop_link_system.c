/* Link-only libSystem identity for the run loop fixture; Alchemy replaces it
 * by install name and these bodies never run. */
int printf(const char *format, ...) { (void)format; return 0; }
#define F(name) long name(void) { return 0; }
F(usleep) F(pthread_create) F(pthread_join)
F(dispatch_get_global_queue) F(dispatch_queue_create) F(dispatch_async) F(dispatch_barrier_async)
F(dispatch_barrier_sync) F(dispatch_apply) F(dispatch_time) F(dispatch_release) F(dispatch_resume)
F(dispatch_group_create) F(dispatch_group_async) F(dispatch_group_notify) F(dispatch_group_wait)
F(dispatch_source_create) F(dispatch_source_set_event_handler) F(dispatch_source_set_cancel_handler)
F(dispatch_source_set_timer) F(dispatch_source_cancel) F(dispatch_source_get_data) F(dispatch_source_merge_data)
F(_Block_copy) F(_Block_release) F(_Block_object_assign) F(_Block_object_dispose)
char _dispatch_main_q[128], _dispatch_queue_attr_concurrent[64];
char _dispatch_source_type_timer[16], _dispatch_source_type_data_add[16];
void *_NSConcreteGlobalBlock[32], *_NSConcreteStackBlock[32];
