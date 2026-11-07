#ifndef ALCHEMY_SYS_QOS_H
#define ALCHEMY_SYS_QOS_H
/* Independently authored Darwin <sys/qos.h>: the quality-of-service classes. */
typedef unsigned int qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0x21u
#define QOS_CLASS_USER_INITIATED 0x19u
#define QOS_CLASS_DEFAULT 0x15u
#define QOS_CLASS_UTILITY 0x11u
#define QOS_CLASS_BACKGROUND 0x09u
#define QOS_CLASS_UNSPECIFIED 0x00u
#define QOS_MIN_RELATIVE_PRIORITY (-15)
#ifdef __cplusplus
extern "C" {
#endif
qos_class_t qos_class_self(void);
qos_class_t qos_class_main(void);
#ifdef __cplusplus
}
#endif
#endif
