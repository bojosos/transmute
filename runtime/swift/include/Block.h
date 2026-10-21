#ifndef ALCHEMY_BLOCK_H
#define ALCHEMY_BLOCK_H
/* Published Blocks runtime entry points (the Clang Blocks ABI). Alchemy's
   libSystem provider implements them. Independently authored; no SDK
   content. */
#ifdef __cplusplus
extern "C" {
#endif
void* _Block_copy(const void* block);
void _Block_release(const void* block);
void _Block_object_assign(void* destination, const void* object, const int flags);
void _Block_object_dispose(const void* object, const int flags);
extern void* _NSConcreteGlobalBlock[32];
extern void* _NSConcreteStackBlock[32];
#ifdef __cplusplus
}
#endif
#define Block_copy(...) ((__typeof(__VA_ARGS__))_Block_copy((const void*)(__VA_ARGS__)))
#define Block_release(...) _Block_release((const void*)(__VA_ARGS__))
#endif
