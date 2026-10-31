# Native display experiment

The latest hardware SpringBoard SIGTRAP maps to the original FrontBoard assertion
`failed to find the main CADisplay`. The missing Objective-C trampoline library
failure is no longer the current stop. See [hardware observations](hardware-tests.md).

The next experiment publishes the already restored factory framebuffer through the
original iPadOS 17.7.11 `IOMobileFramebufferLegacy` implementation. It preserves the
factory boot chain, existing USB root, read-only SystemOS/OS union and selected
external Data. The shared generic CPU backend comes from UniXNU/GopFramebuffer
commit `cf3b42979a9c44219e7bb149543e6fc3388b58ed`; the later SoC-specific scanout
implementation is not part of this experiment. Lifecycle and original ABI binding
code are adapted from UniXNU-sheng under BSD-2-Clause-Patent.

`tools/xnu_screen.py --display` explicitly selects this route for the prelinked main
OS. Without that switch, the existing screen/restore paths do not publish a display.
Before launchd, `CoachzDisplay.cpp` loads the original IOSurface and MobileGraphics
families, registers its own derived class after its original superclass, and
publishes an IOPlatformDevice beneath the actual arm-io device-tree node. Mode,
stride, physical framebuffer address and mapping length come from `PE_current_console`;
scale is two and rotation remains the actual unrotated mode.

The owned backend copies accepted single-layer BGRA IOSurfaces to that same firmware
framebuffer on the CPU. An optional work-loop timer supplies 60 Hz software refresh
notifications through the original family. This is not a hardware vblank source or
GPU/Metal implementation. Unsupported abstract backend calls panic with the slot,
caller, startup stage and progress counters. Existing screen diagnostics remain on.

The build qualifies all 358 original virtual slots, constructor/destructor ancestry,
surface getters and refresh entry points against the accepted kernel. After prelink,
it requires the original IOSurface/MobileGraphics extents and complete MobileGraphics
executable hash to match that reference. Only the owned class initializer's section
registration changes; its executable bytes stay unchanged. Apple component code and
vtables are not patched by this backend.

Initial host checks cover the actual compiled constructor object, the shared pixel
copy/bounds tests, and explicit display selection without regressing external Data
or injecting a RAM seed. These checks do not establish a display service on hardware,
a SpringBoard UI, rendering correctness or input support. The next kernel prefix
must have the same four-partition GPT as the accepted full image and stop exactly
before the OS partition, preserving OS, SystemOS and existing Data contents.
