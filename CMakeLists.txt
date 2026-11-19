cmake_minimum_required(VERSION 3.22)
project(GTAIV_AppleMusic_Radio VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime (/MT) so both .asi and standalone executables have zero VC++ redistributable conflicts
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(STATUS "Configuring 32-bit (Win32/x86) Stage C targets: IVRadioProbe.asi + wmf_selftest.exe + ipc_protocol_selftest.exe")

    add_library(minhook STATIC
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde32.c
        third_party/minhook/src/hde/hde64.c
    )
    target_include_directories(minhook PUBLIC third_party/minhook/include)

    add_library(IVRadioProbe SHARED
        src/dllmain.cpp
        src/async_logger.cpp
        src/stage_b_source.cpp
        src/stage_c_ipc.cpp
        src/wmf_wrappers.cpp
    )
    target_include_directories(IVRadioProbe PRIVATE src)
    target_link_libraries(IVRadioProbe PRIVATE minhook wmvcore ole32 uuid)
    set_target_properties(IVRadioProbe PROPERTIES
        OUTPUT_NAME "IVRadioProbe"
        SUFFIX ".asi"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(wmf_selftest
        tests/wmf_selftest.cpp
    )
    target_include_directories(wmf_selftest PRIVATE src)
    target_link_libraries(wmf_selftest PRIVATE wmvcore ole32 uuid)
    set_target_properties(wmf_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(ipc_protocol_selftest
        tests/ipc_protocol_selftest.cpp
        src/async_logger.cpp
        src/stage_c_ipc.cpp
    )
    target_include_directories(ipc_protocol_selftest PRIVATE src)
    target_link_libraries(ipc_protocol_selftest PRIVATE ole32 uuid winmm)
    set_target_properties(ipc_protocol_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
else()
    message(STATUS "Configuring 64-bit (x64) Stage C/D/E companion targets: iv_stage_c_producer.exe + iv_stage_d_probe.exe + stage_d_offline_selftest.exe + iv_stage_d_producer.exe + stage_d_bridge_selftest.exe + iv_apple_music_radio.exe + stage_e_offline_selftest.exe")

    add_executable(iv_stage_c_producer
        tools/stage_c_producer.cpp
    )
    target_include_directories(iv_stage_c_producer PRIVATE src)
    target_link_libraries(iv_stage_c_producer PRIVATE winmm)
    set_target_properties(iv_stage_c_producer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(iv_stage_d_probe
        tools/stage_d_audio_probe.cpp
        src/stage_d_capture.cpp
    )
    target_include_directories(iv_stage_d_probe PRIVATE src)
    target_link_libraries(iv_stage_d_probe PRIVATE mmdevapi ole32 avrt uuid)
    if(MSVC)
        target_compile_options(iv_stage_d_probe PRIVATE /W4 /WX)
    endif()
    set_target_properties(iv_stage_d_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(stage_d_offline_selftest
        tests/stage_d_offline_selftest.cpp
        src/stage_d_capture.cpp
    )
    target_include_directories(stage_d_offline_selftest PRIVATE src)
    target_link_libraries(stage_d_offline_selftest PRIVATE mmdevapi ole32 avrt uuid)
    if(MSVC)
        target_compile_options(stage_d_offline_selftest PRIVATE /W4 /WX)
    endif()
    set_target_properties(stage_d_offline_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(iv_stage_d_producer
        tools/stage_d_producer.cpp
        src/stage_d_capture.cpp
        src/stage_d_timeline.cpp
    )
    target_include_directories(iv_stage_d_producer PRIVATE src)
    target_link_libraries(iv_stage_d_producer PRIVATE mmdevapi ole32 avrt uuid winmm)
    if(MSVC)
        target_compile_options(iv_stage_d_producer PRIVATE /W4 /WX)
    endif()
    set_target_properties(iv_stage_d_producer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(stage_d_bridge_selftest
        tests/stage_d_bridge_selftest.cpp
        src/stage_d_timeline.cpp
    )
    target_include_directories(stage_d_bridge_selftest PRIVATE src)
    if(MSVC)
        target_compile_options(stage_d_bridge_selftest PRIVATE /W4 /WX)
    endif()
    set_target_properties(stage_d_bridge_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(iv_apple_music_radio WIN32
        tools/stage_e_launcher.cpp
        src/stage_e_engine.cpp
        src/stage_e_diagnostics.cpp
        src/stage_e_routing_isolator.cpp
        src/stage_d_capture.cpp
        src/stage_d_timeline.cpp
    )
    target_include_directories(iv_apple_music_radio PRIVATE src)
    target_link_libraries(iv_apple_music_radio PRIVATE
        user32 gdi32 shell32 comctl32 comdlg32 mmdevapi ole32 avrt uuid winmm bcrypt setupapi advapi32
    )
    if(MSVC)
        target_compile_options(iv_apple_music_radio PRIVATE /W4 /WX)
    endif()
    set_target_properties(iv_apple_music_radio PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(stage_e_offline_selftest
        tests/stage_e_offline_selftest.cpp
        src/stage_e_engine.cpp
        src/stage_e_diagnostics.cpp
        src/stage_e_routing_isolator.cpp
        src/stage_d_capture.cpp
        src/stage_d_timeline.cpp
    )
    target_include_directories(stage_e_offline_selftest PRIVATE src)
    target_link_libraries(stage_e_offline_selftest PRIVATE
        mmdevapi ole32 avrt uuid winmm bcrypt setupapi advapi32
    )
    if(MSVC)
        target_compile_options(stage_e_offline_selftest PRIVATE /W4 /WX)
    endif()
    set_target_properties(stage_e_offline_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_executable(Setup WIN32 tools/setup.cpp)
    target_link_libraries(Setup PRIVATE bcrypt comdlg32 shell32 ole32 uuid version setupapi)
    if(MSVC)
        target_compile_options(Setup PRIVATE /W4 /WX)
    endif()
    set_target_properties(Setup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
