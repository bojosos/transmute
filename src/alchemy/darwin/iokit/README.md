# IOKit

**Covers:** the read-only IOKit registry describing the host
(`iokit_registry.cpp`: platform expert, Ethernet interfaces, and one
IOPCIDevice with an IOGraphicsAccelerator2 child per host GPU, the
accelerator's entry ID being the GPU's MTLDevice.registryID; traversal by
child/parent iterators, paths, IORegistryCreateIterator with enter/exit, class
queries, busy state and IOMainPort), HID managers with no devices (properties,
matching, the dispatch-queue model with its cancel handler), power assertions
(by name, description or property dictionary; retained, re-levelled,
listed), power sources, notification gaps, ForceFeedback, two DiskArbitration constants (`iokit.cpp`,
fill-only), wireless, controller, location and authentication frameworks
(`peripherals.cpp`), and Chrome's IOKit gaps (`iokit_gaps.cpp`).

**Headless:** `iokit_headless.cpp` (alchemy.exe only), empty `iokit_fx.cpp`
in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** `host::system` (machine UUID, power requests, power
status, sleep and power-source notifications), the registry's adapter
queries, and the GPU list of `darwin/metal/gpu_identity.hpp` (Vulkan
physical devices; empty without Vulkan).

**Unsupported:** HID devices, elements, values and IOConnect calls never
exist; their calls fail with IOKit's error codes or stop for an object that
cannot exist. Registry properties cannot be set (no driver:
kIOReturnUnsupported), and entries have no plane locations. The HID event-system client
(`IOHIDEventSystemClientCreate` and the service and event calls, which Electron
reads thermal sensors with) is unavailable: no client is created, and the calls
on a client, service or event answer empty for NULL and stop for anything else.

## Moved from darwin/identities/

* `empty_registry.cpp`: headless, every build: matching dictionaries are real, and the empty registry finds no service.
* `tests/iokit_registry_tests.cpp`: the registry as a renderer plug-in's images query it (ctest `alchemy_gui_iokit_registry`).
