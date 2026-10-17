cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(WIN32)
    # setup_win32.c: the first-run window of a shared build (recomp_setup.h),
    # with the system's controls and file dialog.
    add_library(platform STATIC setup_win32.c)
    target_include_directories(platform PUBLIC ${XBOXRECOMP_SRC_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(platform PUBLIC comctl32 ole32 shell32 uuid)
else()
    # host_sdl.c: the game window, keyboard and gamepads (one SDL thread), and
    # the Win32 calls the shared code reads them through.
    # setup_sdl.c: the first-run window of a shared build (recomp_setup.h).
    add_library(platform STATIC win32_compat.c host_sdl.c setup_sdl.c)
    # posix_include: <windows.h> and <dbghelp.h> that forward to the POSIX
    # Win32 layer, so shared sources keep their includes.
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/posix_include
    )
    find_package(Threads REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SETUP_FONT REQUIRED IMPORTED_TARGET freetype2 fontconfig)
    # dl: SymFromAddr is dladdr. m: the setup window's easing.
    target_link_libraries(platform PUBLIC Threads::Threads SDL2::SDL2 ${CMAKE_DL_LIBS}
                          PRIVATE PkgConfig::SETUP_FONT m)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
