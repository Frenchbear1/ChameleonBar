cmake_minimum_required(VERSION 3.24)
project(ChameleonBar LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ChameleonBar targets Windows 11.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ChameleonBar WIN32
    app.rc
    src/main.cpp
    src/ui.cpp
    src/color_sampler.cpp
    src/settings.cpp
    src/surface_publisher.cpp
    src/window_activity.cpp
    src/xaml_bridge_client.cpp
)
target_include_directories(ChameleonBar PRIVATE src)
target_compile_definitions(ChameleonBar PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ChameleonBar PRIVATE
    user32 gdi32 gdiplus shell32 dwmapi advapi32 ole32 uuid
)

add_library(ChameleonBarXamlBridge SHARED
    bridge/xaml_bridge.cpp
    bridge/exports.def
)
set_target_properties(ChameleonBarXamlBridge PROPERTIES
    OUTPUT_NAME "ChameleonBar.XamlBridge"
    PREFIX ""
)
target_compile_definitions(ChameleonBarXamlBridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ChameleonBarXamlBridge PRIVATE
    ole32 oleaut32 runtimeobject gdi32 user32 uuid
)

add_executable(sampler_tests
    tests/sampler_tests.cpp
    src/color_sampler.cpp
    src/surface_publisher.cpp
    src/window_activity.cpp
    src/settings.cpp
)
target_include_directories(sampler_tests PRIVATE src)
target_compile_definitions(sampler_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(sampler_tests PRIVATE user32 dwmapi shell32 advapi32)

enable_testing()
add_test(NAME sampler_tests COMMAND sampler_tests)

if(MSVC)
    foreach(target ChameleonBar ChameleonBarXamlBridge sampler_tests)
        # /Brepro: byte-identical output for identical source, so the
        # committed binaries in dist/ only change when the code does.
        target_compile_options(${target} PRIVATE /W4 /EHsc /utf-8 /Brepro)
        target_link_options(${target} PRIVATE /Brepro)
    endforeach()
endif()

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}
        "${CMAKE_BINARY_DIR}/bin/${config}")
endforeach()
