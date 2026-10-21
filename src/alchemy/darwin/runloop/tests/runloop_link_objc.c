/* Link-only libobjc identity for the run loop fixture; Alchemy replaces it
 * by install name and these bodies never run. */
#define F(name) long name(void) { return 0; }
F(objc_getClass) F(sel_registerName) F(objc_msgSend)
