# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    # Host device choice and per-control bindings, from the config file the
    # input UI writes (tools/input_ui). Used by both input paths: the XAPI
    # replacement in src/hle and the USB gamepad in src/usb.
    input_bindings.c
    # The mouse: buttons as binding sources, raw movement for a title's own
    # mouse-look hook. Raw input on the game window, which src/hle hands over.
    recomp_mouse.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput user32)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
