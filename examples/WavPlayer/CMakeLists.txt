cmake_minimum_required(VERSION 3.15)

set (TARGET_NAME WavPlayer)

if (DEFINED PROJECT_NAME)
    message ("Adding ${TARGET_NAME} to the ${PROJECT_NAME} project")
else()
    file(READ ../../VERSION.md CURRENT_VERSION)
    project(${TARGET_NAME} VERSION ${CURRENT_VERSION})
    add_subdirectory(../../modules/juce ./cmake_build_juce)
    add_subdirectory(../../modules ./cmake_build_tracktion)
endif()

if (MSVC)
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE INTERNAL "")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS YES)
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Enable Module Source Groups" ON)

juce_add_console_app(${TARGET_NAME})
juce_generate_juce_header(${TARGET_NAME})

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

set_target_properties(${TARGET_NAME} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)

target_sources(${TARGET_NAME} PRIVATE
    ${TARGET_NAME}.cpp
    ${TARGET_NAME}.h)

target_compile_definitions(${TARGET_NAME} PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(${TARGET_NAME} PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
    CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${TARGET_NAME} PRIVATE "$<$<CONFIG:Release>:-Werror>")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(${TARGET_NAME} PRIVATE "/W4")
    target_compile_options(${TARGET_NAME} PRIVATE "$<$<CONFIG:Release>:/WX>")
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_compile_options(${TARGET_NAME} PRIVATE "-fno-aligned-allocation")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(${TARGET_NAME} PRIVATE "-latomic")
  if (NOT ${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "aarch64")
    target_link_options(${TARGET_NAME} PRIVATE "-m64")
  endif()
endif()
