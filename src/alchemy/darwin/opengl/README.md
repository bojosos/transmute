# OpenGL and AGL

`/System/Library/Frameworks/OpenGL.framework` and `AGL.framework`
(alchemy-fx). The headless OpenGL provider (CGL, NSOpenGL creation failing)
is in `darwin/identities/frameworks.cpp`.

* **Real:** the AGL identity (libqcocoa links it without importing from it).
* **Unavailable, as documented:** no OpenGL context can exist: answers are
  those of OpenGL without a current context (`glGetString` returns NULL,
  `glGetIntegerv` leaves its output untouched, `glGetError` reports
  GL_INVALID_OPERATION), and the NSOpenGLPixelFormat / NSOpenGLContext
  initializers release the receiver and return nil, AppKit's answer for an
  unsatisfiable request (`no_context.cpp`). Metal is the GPU API Alchemy
  implements.
* **Host dependencies:** none.
* **Registration:** installed by the support kit (`kit/support/install.cpp`) at
  its old position (Qt's Cocoa plugin first imported it).
* **Tests:** `tests/opengl_tests.cpp` (ctest `alchemy_gui_opengl`).
