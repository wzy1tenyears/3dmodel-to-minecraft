cmake_minimum_required(VERSION 3.21)

project(3dmodel-to-minecraft LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "3dmodel-to-minecraft currently requires Windows and MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure with the Visual Studio x64 platform (-A x64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CODEX_APP_SOURCES
    main.cpp
    app/cli_args.cpp
    native/nbt.cpp
    native/palette.cpp
    native/world_ops.cpp
    native/glb_surface_importer.cpp
    native/glb_textured_importer.cpp
    native/obj_textured_importer.cpp
    native/preview_mesh.cpp
    native/preview_cache.cpp
    native/preview_directstorage.cpp
    native/preview_gpu_lod.cpp
    native/preview_gpu_lod_d3d12.cpp
    native/preview_gpu_lod_dispatch.cpp
    native/preview_d3d11_renderer.cpp
    native/preview_d3d12_renderer.cpp
    native/preview_vulkan_renderer.cpp
    native/preview_renderer.cpp
    third_party/miniz.c
    third_party/miniz_tdef.c
    third_party/miniz_tinfl.c
)

set(CODEX_INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/directstorage/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan_headers/include"
)

set(CODEX_WINDOWS_LIBRARIES
    User32
    Gdi32
    Comctl32
    OpenGL32
    Shell32
    Comdlg32
    Ole32
    Winmm
    d3d11
    d3d12
    d3dcompiler
    dxgi
)

function(add_codex_executable target_name output_name entry_define)
    # Both variants intentionally remain console-subsystem executables because
    # main.cpp exposes wmain; the GUI variant hides its console at runtime.
    add_executable("${target_name}" ${CODEX_APP_SOURCES})

    set_target_properties("${target_name}" PROPERTIES
        OUTPUT_NAME "${output_name}"
    )

    target_include_directories("${target_name}" PRIVATE
        ${CODEX_INCLUDE_DIRECTORIES}
    )
    target_compile_definitions("${target_name}" PRIVATE
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
        "${entry_define}"
    )
    target_compile_options("${target_name}" PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        /utf-8
    )
    target_link_libraries("${target_name}" PRIVATE
        ${CODEX_WINDOWS_LIBRARIES}
    )

    foreach(runtime_dll IN ITEMS dstorage.dll dstoragecore.dll)
        add_custom_command(TARGET "${target_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/directstorage/bin/x64/${runtime_dll}"
                "$<TARGET_FILE_DIR:${target_name}>/${runtime_dll}"
            VERBATIM
        )
    endforeach()
endfunction()

add_codex_executable(
    codex_gui
    3dmodel-to-minecraft-gui
    CODEX_GUI_EXE
)

add_codex_executable(
    codex_cli
    3dmodel-to-minecraft-cli
    CODEX_CLI_EXE
)
