cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C CXX)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (Xbox kernel ordinals → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
# Linux: the POSIX and GNU extensions the host layers use (pthread_setname_np,
# memfd_create, MAP_FIXED_NOREPLACE, dladdr, clock_nanosleep).
if(NOT WIN32)
    add_compile_definitions(_GNU_SOURCE)
endif()

# An implicit declaration is an error, everywhere, in every configuration.
#
# C89's rule is that an undeclared function returns int, and on this target
# that silently truncates a 64-bit return value to 32 bits and sign extends
# it. Seven files used getenv, malloc or calloc without <stdlib.h> and so
# received corrupted pointers: RECOMP_WATCHDOG_SECS faulted on
# 0xFFFFFFFFA94859D5 about one run in three and was inert the rest of the
# time, and two D3D8 draw paths were allocating through a truncated malloc.
#
# It was a warning the whole time (C4013, alongside C4047 for the assignment).
# A warning among hundreds in a build this size is not a signal, so it is an
# error now. C4047 stays a warning: it fires on legitimate casts elsewhere.
if(MSVC)
    add_compile_options(/we4013)
else()
    add_compile_options(-Werror=implicit-function-declaration)
endif()

# No errno from the math functions (nothing here reads it), so sqrtf and
# friends become instructions instead of libm calls. Beyond speed, glibc 2.43
# versioned sqrtf anew, and one call from a build on a new system was enough
# to make the shared runtime refuse to start on any older one.
if(NOT MSVC)
    add_compile_options(-fno-math-errno)
endif()

# The lifted game as a module the runtime loads at run time
# (src/kernel/recomp_module.h): the executable then holds no game code and can
# be shared, and the module is built from the player's own disc. Builds the
# runtime libraries with RECOMP_MODULE_HOST.
#
# XBOXRECOMP_REGS_IN_MODULE: the module, not the executable, owns the guest
# register block (xbox_memory_layout.h). Windows needs it -- a DLL cannot
# import an executable's __declspec(thread) data -- and it works on Linux too
# (the block then sits in the module's static TLS); Linux keeps the block in
# the executable by default, its proven arrangement. The title's module and
# executable need RECOMP_REGS_IN_MODULE as well (see its CMakeLists).
#
# On Windows the module is a DLL that imports the runtime from the
# executable (its import library) and owns the register block.
option(XBOXRECOMP_GAME_MODULE "Load the lifted game as a shared module" OFF)
if(WIN32)
    set(_regs_in_module_default ON)
else()
    set(_regs_in_module_default OFF)
endif()
option(XBOXRECOMP_REGS_IN_MODULE "The game module owns the guest registers"
       ${_regs_in_module_default})
if(XBOXRECOMP_GAME_MODULE)
    if(WIN32 AND NOT XBOXRECOMP_REGS_IN_MODULE)
        message(FATAL_ERROR "A Windows game module needs XBOXRECOMP_REGS_IN_MODULE: "
                            "a DLL cannot import the executable's thread-local registers")
    endif()
    add_compile_definitions(RECOMP_MODULE_HOST)
    if(XBOXRECOMP_REGS_IN_MODULE)
        add_compile_definitions(RECOMP_REGS_IN_MODULE)
    endif()
endif()

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Player settings, read by the d3d backend, the kernel and the HLE, so it
# comes before all three and depends on none of them.
add_subdirectory(src/config)

# The launcher a title ships beside its game. Defines recomp_add_launcher
# for a title's CMakeLists to call; builds nothing on its own.
add_subdirectory(src/launcher)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# XDK functions replaced by name (tools/recomp/hle.py)
add_subdirectory(src/hle)

# Frame replay tool: replays a D3D8 capture into the host renderer with no
# game running (src/hle/d3d8_capture.h). Windows only, like the D3D11 backend.
if(WIN32)
    add_subdirectory(src/replay)
endif()

# ── Tests ─────────────────────────────────────────────────────

# Offline tests that need no game files; run with ctest. Only when this is the
# top-level project, so a title that pulls the toolkit in with
# add_subdirectory() does not build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests/nv2a_vsh)
    add_subdirectory(tests/d3d8_capture)        # portable: the capture container
    if(WIN32)
        add_subdirectory(tests/nv2a_vsh_hlsl)   # needs D3DCompile
        # Built under src/d3d (tests/d3d8_smoke); pure parsing, no device.
        add_test(NAME d3d8_final_default COMMAND d3d8_final_default)
    endif()
endif()

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
    xbox_hle
)

