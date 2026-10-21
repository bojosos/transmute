/* Link-only CoreFoundation identity for the run loop fixture; Alchemy
 * replaces it by install name and these bodies never run. */
#define F(name) long name(void) { return 0; }
F(CFAbsoluteTimeGetCurrent) F(CFRelease) F(CFArrayGetCount)
F(CFRunLoopGetMain) F(CFRunLoopGetCurrent) F(CFRunLoopRunInMode) F(CFRunLoopStop) F(CFRunLoopWakeUp)
F(CFRunLoopAddTimer) F(CFRunLoopTimerCreate) F(CFRunLoopTimerCreateWithHandler) F(CFRunLoopTimerInvalidate)
F(CFRunLoopTimerIsValid) F(CFRunLoopSourceCreate) F(CFRunLoopAddSource) F(CFRunLoopSourceSignal)
F(CFRunLoopSourceInvalidate) F(CFRunLoopObserverCreateWithHandler) F(CFRunLoopAddObserver)
F(CFRunLoopObserverInvalidate)
const void *kCFRunLoopDefaultMode, *kCFRunLoopCommonModes;
