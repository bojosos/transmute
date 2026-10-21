#ifndef ALCHEMY_OBJC_RUNTIME_H
#define ALCHEMY_OBJC_RUNTIME_H
/* Independently authored declarations of the published Objective-C runtime
   interface. No SDK content. */
#include <objc/objc.h>
typedef struct objc_method* Method;
typedef struct objc_ivar* Ivar;
typedef struct objc_category* Category;
typedef struct objc_property* objc_property_t;
#ifdef __OBJC__
@class Protocol;
#else
typedef struct objc_object Protocol;
#endif
struct objc_method_description { SEL name; char* types; };
typedef struct { const char* name; const char* value; } objc_property_attribute_t;
struct mach_header;
struct objc_image_info;
#ifdef __cplusplus
extern "C" {
#endif
OBJC_EXPORT id object_copy(id obj, size_t size);
OBJC_EXPORT id object_dispose(id obj);
OBJC_EXPORT Class object_getClass(id obj);
OBJC_EXPORT Class object_setClass(id obj, Class cls);
OBJC_EXPORT BOOL object_isClass(id obj);
OBJC_EXPORT id object_getIvar(id obj, Ivar ivar);
OBJC_EXPORT void object_setIvar(id obj, Ivar ivar, id value);
OBJC_EXPORT Class objc_getClass(const char* name);
OBJC_EXPORT Class objc_getMetaClass(const char* name);
OBJC_EXPORT Class objc_lookUpClass(const char* name);
OBJC_EXPORT Class objc_getRequiredClass(const char* name);
OBJC_EXPORT int objc_getClassList(Class* buffer, int count);
OBJC_EXPORT Class* objc_copyClassList(unsigned int* count);
OBJC_EXPORT const char* class_getName(Class cls);
OBJC_EXPORT BOOL class_isMetaClass(Class cls);
OBJC_EXPORT Class class_getSuperclass(Class cls);
OBJC_EXPORT Class class_setSuperclass(Class cls, Class superclass);
OBJC_EXPORT int class_getVersion(Class cls);
OBJC_EXPORT void class_setVersion(Class cls, int version);
OBJC_EXPORT size_t class_getInstanceSize(Class cls);
OBJC_EXPORT Ivar class_getInstanceVariable(Class cls, const char* name);
OBJC_EXPORT Ivar class_getClassVariable(Class cls, const char* name);
OBJC_EXPORT Ivar* class_copyIvarList(Class cls, unsigned int* count);
OBJC_EXPORT Method class_getInstanceMethod(Class cls, SEL name);
OBJC_EXPORT Method class_getClassMethod(Class cls, SEL name);
OBJC_EXPORT IMP class_getMethodImplementation(Class cls, SEL name);
OBJC_EXPORT BOOL class_respondsToSelector(Class cls, SEL sel);
OBJC_EXPORT Method* class_copyMethodList(Class cls, unsigned int* count);
OBJC_EXPORT BOOL class_conformsToProtocol(Class cls, Protocol* protocol);
OBJC_EXPORT Protocol* __unsafe_unretained* class_copyProtocolList(Class cls, unsigned int* count);
OBJC_EXPORT objc_property_t class_getProperty(Class cls, const char* name);
OBJC_EXPORT objc_property_t* class_copyPropertyList(Class cls, unsigned int* count);
OBJC_EXPORT const uint8_t* class_getIvarLayout(Class cls);
OBJC_EXPORT const uint8_t* class_getWeakIvarLayout(Class cls);
OBJC_EXPORT BOOL class_addMethod(Class cls, SEL name, IMP imp, const char* types);
OBJC_EXPORT IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char* types);
OBJC_EXPORT BOOL class_addIvar(Class cls, const char* name, size_t size, uint8_t alignment, const char* types);
OBJC_EXPORT BOOL class_addProtocol(Class cls, Protocol* protocol);
OBJC_EXPORT id class_createInstance(Class cls, size_t extraBytes);
OBJC_EXPORT id objc_constructInstance(Class cls, void* bytes);
OBJC_EXPORT void* objc_destructInstance(id obj);
OBJC_EXPORT Class objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes);
OBJC_EXPORT void objc_registerClassPair(Class cls);
OBJC_EXPORT Class objc_duplicateClass(Class original, const char* name, size_t extraBytes);
OBJC_EXPORT void objc_disposeClassPair(Class cls);
OBJC_EXPORT SEL method_getName(Method m);
OBJC_EXPORT IMP method_getImplementation(Method m);
OBJC_EXPORT const char* method_getTypeEncoding(Method m);
OBJC_EXPORT unsigned int method_getNumberOfArguments(Method m);
OBJC_EXPORT char* method_copyReturnType(Method m);
OBJC_EXPORT char* method_copyArgumentType(Method m, unsigned int index);
OBJC_EXPORT struct objc_method_description* method_getDescription(Method m);
OBJC_EXPORT IMP method_setImplementation(Method m, IMP imp);
OBJC_EXPORT void method_exchangeImplementations(Method m1, Method m2);
OBJC_EXPORT const char* ivar_getName(Ivar v);
OBJC_EXPORT const char* ivar_getTypeEncoding(Ivar v);
OBJC_EXPORT ptrdiff_t ivar_getOffset(Ivar v);
OBJC_EXPORT const char* property_getName(objc_property_t property);
OBJC_EXPORT const char* property_getAttributes(objc_property_t property);
OBJC_EXPORT Protocol* objc_getProtocol(const char* name);
OBJC_EXPORT Protocol* __unsafe_unretained* objc_copyProtocolList(unsigned int* count);
OBJC_EXPORT BOOL protocol_conformsToProtocol(Protocol* proto, Protocol* other);
OBJC_EXPORT BOOL protocol_isEqual(Protocol* proto, Protocol* other);
OBJC_EXPORT const char* protocol_getName(Protocol* proto);
OBJC_EXPORT struct objc_method_description protocol_getMethodDescription(Protocol* proto, SEL aSel, BOOL isRequiredMethod, BOOL isInstanceMethod);
OBJC_EXPORT const char* class_getImageName(Class cls);
OBJC_EXPORT const char** objc_copyImageNames(unsigned int* outCount);
OBJC_EXPORT const char** objc_copyClassNamesForImage(const char* image, unsigned int* outCount);
OBJC_EXPORT void objc_enumerationMutation(id obj);
OBJC_EXPORT void objc_setEnumerationMutationHandler(void (*handler)(id));
OBJC_EXPORT IMP imp_implementationWithBlock(id block);
OBJC_EXPORT id imp_getBlock(IMP anImp);
OBJC_EXPORT BOOL imp_removeBlock(IMP anImp);
OBJC_EXPORT id objc_loadWeak(id* location);
OBJC_EXPORT id objc_storeWeak(id* location, id obj);
typedef uintptr_t objc_AssociationPolicy;
enum {
    OBJC_ASSOCIATION_ASSIGN = 0,
    OBJC_ASSOCIATION_RETAIN_NONATOMIC = 1,
    OBJC_ASSOCIATION_COPY_NONATOMIC = 3,
    OBJC_ASSOCIATION_RETAIN = 01401,
    OBJC_ASSOCIATION_COPY = 01403
};
OBJC_EXPORT void objc_setAssociatedObject(id object, const void* key, id value, objc_AssociationPolicy policy);
OBJC_EXPORT id objc_getAssociatedObject(id object, const void* key);
OBJC_EXPORT void objc_removeAssociatedObjects(id object);
typedef BOOL (*objc_hook_getClass)(const char* name, Class* outClass);
OBJC_EXPORT void objc_setHook_getClass(objc_hook_getClass newValue, objc_hook_getClass* outOldValue);
typedef BOOL (*objc_hook_getImageName)(Class cls, const char** outImageName);
OBJC_EXPORT void objc_setHook_getImageName(objc_hook_getImageName newValue, objc_hook_getImageName* outOldValue);
typedef void (*objc_func_loadImage)(const struct mach_header* header);
OBJC_EXPORT void objc_addLoadImageFunc(objc_func_loadImage func);
typedef const char* (*objc_hook_lazyClassNamer)(Class cls);
OBJC_EXPORT void objc_setHook_lazyClassNamer(objc_hook_lazyClassNamer newValue, objc_hook_lazyClassNamer* oldOutValue);
typedef Class (*_objc_swiftMetadataInitializer)(Class cls, void* arg);
OBJC_EXPORT Class _objc_realizeClassFromSwift(Class cls, void* previously);
#ifdef __cplusplus
}
#endif
#endif
