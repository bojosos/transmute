#ifndef ALCHEMY_SWIFT_TARGET_CONDITIONALS_H
#define ALCHEMY_SWIFT_TARGET_CONDITIONALS_H
/* macOS arm64 target conditionals. Independently authored; no SDK content. */
#include_next <TargetConditionals.h>
#define TARGET_OS_UNIX 0
#define TARGET_OS_MACCATALYST 0
#define TARGET_OS_VISION 0
#define TARGET_OS_XR 0
#define TARGET_OS_BRIDGE 0
#define TARGET_OS_DRIVERKIT 0
#define TARGET_OS_LINUX 0
#define TARGET_OS_WINDOWS 0
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN 0
#define TARGET_RT_64_BIT 1
#define TARGET_CPU_ARM64 1
#define TARGET_CPU_X86_64 0
#endif
