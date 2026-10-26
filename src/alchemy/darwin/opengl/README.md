# OpenGL and AGL

`/System/Library/Frameworks/OpenGL.framework` and `AGL.framework` (alchemy-fx), over the host's
OpenGL driver (`host::gl`: WGL on win32). The decision and the estimate behind it (WGL rather than
OpenGL over Vulkan) are in the frameworks wave's notes, 2026-10-25.

* **Real:**
  * **The gl\* entry points** (`gl_exports.cpp`, 965 names in `gl_functions.inc`): OpenGL 1.0–4.1, both
    profiles, and the EXT/ARB/APPLE names macOS keeps (EXT_framebuffer_object/blit/multisample,
    ARB_vertex_buffer_object, ARB_multitexture, the EXT blend names, APPLE_vertex_array_object,
    APPLE_flush_render). Each one calls the driver's function of the same name, or the core function
    an extension name was promoted to.
    * Arguments are read with the Apple arm64 rules and passed with the host's.
    * Pointers pass unchanged, because guest addresses are host addresses in alchemy-fx. That covers
      client arrays, pixel data, mapped buffers and sync objects.
    * `glGetString`/`glGetStringi` copy the driver's strings into guest memory. The strings are the
      driver's own (a core profile reports the driver's version, 4.x).
  * **CGL** (`cgl_contexts.cpp`):
    * pixel formats (one virtual screen, the host GPU; legacy and 3.2/4.1 core profiles);
    * contexts with sharing, retain counts and the per-thread current context;
    * parameters, enables, options and the context lock;
    * renderer info and `CGLErrorString`;
    * `CGLTexImageIOSurface2D` (see below).
  * **AppKit's OpenGL classes** (`nsopengl.cpp`), on CGL as on macOS:
    * NSOpenGLPixelFormat and NSOpenGLContext (`+currentContext`, parameters, `-update`);
    * NSOpenGLView: its context on first use, `-prepareOpenGL` once, and drawing through `-displayLayer:`
      (the context is made current and `-drawRect:` runs).
    * `-setView:` gives a context a drawable: a framebuffer object of the view's size in pixels that stands
      in for framebuffer 0. `glBindFramebuffer(0)` binds it, `glGetIntegerv` reports it as 0, and
      GL_BACK/GL_FRONT name its color attachment.
    * `-flushBuffer` reads the drawable back into an IOSurface shown as the contents of a sublayer of the
      view's layer, which the window compositor shows.
  * **CAOpenGLLayer** (`nsopengl.cpp`; the class is created here, as QuartzCore's):
    * its pixel format and context come from its own `-copyCGLPixelFormatForDisplayMask:` and
      `-copyCGLContextForPixelFormat:`, which subclasses may override;
    * `-display` draws one frame into a drawable of the bounds times `contentsScale` in pixels (framebuffer 0
      while it draws): `-canDrawInCGLContext:...`, then `-drawInCGLContext:...` (by default a flush). The
      frame becomes the layer's own contents as an IOSurface.
    * `-setNeedsDisplay` displays on the main thread's run loop, and an asynchronous layer draws again every
      1/60 s.
* **Different from macOS:**
  * **`CGLTexImageIOSurface2D` copies** the surface's pixels into the texture when it is called, under
    a read lock. On macOS the texture aliases the surface.
  * **`GL_YCBCR_422_APPLE`/`GL_RGB_422_APPLE` data** are expanded to RGB, components unconverted.
  * **A CGL context has no drawable of its own.** It draws into framebuffer objects, and
    `CGLFlushDrawable` flushes.
  * **Calls with no current context** do nothing and answer 0, and `glGetError` answers
    GL_INVALID_OPERATION.
* **Unavailable, as documented:**
  * Off-screen, full-screen, pbuffer, stereo, float-color and software-renderer pixel formats have no
    match. `CGLCreatePBuffer` answers kCGLBadAlloc, and `CGLSetOffScreen`/`CGLSetFullScreen` answer
    kCGLBadOffScreen/kCGLBadFullScreen.
  * `CGLCopyContext` is an explicit stop.
  * CGLMacro.h dispatch through the context object doesn't work: the object holds no dispatch table.
  * An entry point the host driver lacks stops with its name.
  * NSOpenGLContext full-screen and off-screen drawables and `-copyAttributesFromContext:withMask:` stop
    with a message. `-[NSOpenGLPixelFormat initWithData:]` answers nil.
* **Host dependencies:** `host::gl` (contexts, entry points). The gl\* functions themselves are
  called directly: OpenGL is portable.
* **Registration:**
  * `opengl::install_opengl`, the `install_opengl` row of the GUI installer table.
  * The AGL identity is installed by the support kit (`kit/support/install.cpp`, `no_context.cpp`).
  * alchemy.exe keeps the headless `CGLTexImageIOSurface2D` answer (`cgl.cpp`); `cgl_fx.cpp` is its
    empty alchemy-fx twin.
* **Generated table:** edit `gl_api_11.txt` / `gl_api_extra.txt`, then run `python -I gl_functions.py`.
  The names and signatures come from the public OpenGL specifications.
* **Tests:** `tests/opengl_tests.cpp` (ctest `alchemy_gui_opengl`).
  * Contexts of both profiles render into framebuffers and read pixels back.
  * Float, stack and pointer-array arguments, buffers and an IOSurface texture are checked.
  * An NSOpenGLContext draws to an NSView and flushes into its sublayer.
  * Skipped without a host driver.
