#ifndef ALCHEMY_OBJC_OBJC_H
#define ALCHEMY_OBJC_OBJC_H
/* Independently authored declarations of the published Objective-C runtime
   interface. No SDK content. */
#include <objc/objc-api.h>
#include <stdbool.h>
#if !defined(__OBJC__)
typedef struct objc_class* Class;
typedef struct objc_object { Class isa; } *id;
#endif
typedef struct objc_selector* SEL;
typedef void (*IMP)(void);
typedef bool BOOL;
#define OBJC_BOOL_IS_BOOL 1
#if __has_feature(objc_bool)
#define YES __objc_yes
#define NO __objc_no
#else
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#endif
#ifndef Nil
#define Nil 0
#endif
#ifndef nil
#define nil 0
#endif
#if !defined(__OBJC__) || !__has_feature(objc_arc)
#ifndef __strong
#define __strong
#endif
#ifndef __unsafe_unretained
#define __unsafe_unretained
#endif
#ifndef __autoreleasing
#define __autoreleasing
#endif
#endif
OBJC_EXPORT const char* sel_getName(SEL sel);
OBJC_EXPORT SEL sel_registerName(const char* name);
OBJC_EXPORT SEL sel_getUid(const char* str);
OBJC_EXPORT BOOL sel_isMapped(SEL sel);
OBJC_EXPORT const char* object_getClassName(id obj);
OBJC_EXPORT void* object_getIndexedIvars(id obj);
#endif
