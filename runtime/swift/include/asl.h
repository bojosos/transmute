#ifndef ALCHEMY_ASL_H
#define ALCHEMY_ASL_H
/* Independently authored declarations of the published ASL logging API. */
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __asl_object_s* asl_object_t;
typedef asl_object_t aslclient;
typedef asl_object_t aslmsg;
#define ASL_LEVEL_EMERG 0
#define ASL_LEVEL_ALERT 1
#define ASL_LEVEL_CRIT 2
#define ASL_LEVEL_ERR 3
#define ASL_LEVEL_WARNING 4
#define ASL_LEVEL_NOTICE 5
#define ASL_LEVEL_INFO 6
#define ASL_LEVEL_DEBUG 7
int asl_log(asl_object_t client, asl_object_t msg, int level, const char* format, ...) __attribute__((format(printf, 4, 5)));
#ifdef __cplusplus
}
#endif
#endif
