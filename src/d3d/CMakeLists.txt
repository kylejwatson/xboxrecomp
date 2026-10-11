# Xbox D3D8 Compatibility Layer
#   Every host runs the same renderer (d3d8_*.c): the Xbox format table,
#   swizzling, palettes, and the NV2A vertex-program and register-combiner
#   translators. Underneath it:
#     _WIN32 -> Direct3D 11 and D3DCompile
#     POSIX  -> the Direct3D 11 subset it uses, on Vulkan 1.3 (vk/d3d11_vk.c),
#               with D3DCompile on shaderc (vk/d3dcompiler_vk.c)
#   d3d8_gl.c, the old OpenGL stand-in, is no longer built.

set(XBOX_D3D8_SOURCES
    d3d8_device.c
    d3d8_resources.c
    d3d8_shaders.c
    d3d8_states.c
    d3d8_combiners.c
    d3d8_vsh.c
    d3d8_overlay.c
    d3d8_movie.c
    d3d8_screencopy.c
    d3d8_clearmask.c
    d3d8_display.c
)

if(WIN32)
    add_library(xbox_d3d8 STATIC ${XBOX_D3D8_SOURCES})
    # gdi32: the overlay renders its text with GDI rather than an embedded font.
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler gdi32)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
else()
    add_library(xbox_d3d8 STATIC ${XBOX_D3D8_SOURCES}
        vk/d3d11_vk.c
        vk/d3dcompiler_vk.c
    )
    find_package(Vulkan REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
    # The F9 overlay's text (GDI on Windows).
    pkg_check_modules(OVERLAY_FONT REQUIRED IMPORTED_TARGET freetype2 fontconfig)
    # vk/include: <d3d11.h>, <dxgi.h> and <d3dcompiler.h> for this subset.
    target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vk/include)
    target_link_libraries(xbox_d3d8 PUBLIC platform Vulkan::Vulkan SDL2::SDL2
                          PkgConfig::SHADERC PkgConfig::OVERLAY_FONT m)
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_d3d8 PUBLIC xbox_config)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2> /Zi)  # /Zi: see src/kernel/CMakeLists.txt
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
