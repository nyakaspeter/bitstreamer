cmake_minimum_required(VERSION 3.24)

option(BITSTREAMER_BUILD_GUI "Build the Qt 6 desktop application" ON)
option(BITSTREAMER_BUILD_TESTS "Build the automated tests" ON)

# vcpkg manifest features must be chosen before project().
if(BITSTREAMER_BUILD_GUI)
    list(APPEND VCPKG_MANIFEST_FEATURES "gui")
endif()

project(bitstreamer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "bitstreamer targets Windows (WASAPI) only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(FFMPEG REQUIRED)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                        BITSTREAMER_VERSION="${PROJECT_VERSION}")
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /MP /wd4324)
endif()

# ---------------------------------------------------------------- core
add_library(bscore STATIC
    src/core/Common.cpp
    src/core/Common.h
    src/core/codec/Codec.cpp
    src/core/codec/Codec.h
    src/core/transport/Iec61937.cpp
    src/core/transport/Iec61937.h
    src/core/dsp/DriftResampler.cpp
    src/core/dsp/DriftResampler.h
    src/core/dsp/Upmixer.cpp
    src/core/dsp/Upmixer.h
    src/core/engine/BitstreamEngine.cpp
    src/core/engine/BitstreamEngine.h
    src/core/wasapi/WasapiCommon.cpp
    src/core/wasapi/WasapiCommon.h
    src/core/wasapi/Capture.cpp
    src/core/wasapi/Capture.h
    src/core/wasapi/Output.cpp
    src/core/wasapi/Output.h
    src/core/wasapi/SteamSpeakers.cpp
    src/core/wasapi/SteamSpeakers.h
    src/core/pipeline/Pipeline.cpp
    src/core/pipeline/Pipeline.h
)
target_include_directories(bscore PUBLIC src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(bscore PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(bscore PUBLIC ${FFMPEG_LIBRARIES} ole32 avrt propsys advapi32 shell32)


# ----------------------------------------------------------------- gui
if(BITSTREAMER_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

# --------------------------------------------------------------- tests
if(BITSTREAMER_BUILD_TESTS)
    enable_testing()
    add_executable(bs_tests
        tests/test_main.cpp
        tests/test_codec.cpp
        tests/test_engine.cpp
        tests/TestFramework.h
    )
    target_link_libraries(bs_tests PRIVATE bscore)
    add_test(NAME unit COMMAND bs_tests)
    set_tests_properties(unit PROPERTIES TIMEOUT 300)
endif()

# ------------------------------------------------------------- install
install(DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
        DESTINATION .
        FILES_MATCHING PATTERN "av*.dll" PATTERN "sw*.dll")
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION . OPTIONAL)
# License texts of the bundled third-party libraries, as shipped by vcpkg.
foreach(_port ffmpeg qtbase pcre2 double-conversion zlib md4c)
    install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${_port}/copyright"
            DESTINATION licenses RENAME "${_port}.txt" OPTIONAL)
endforeach()
install(DIRECTORY docs DESTINATION . OPTIONAL)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "bitstreamer")
set(CPACK_PACKAGE_FILE_NAME "bitstreamer-${PROJECT_VERSION}-win64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
