# CMakeLists.txt for FusionNet Android NDK
cmake_minimum_required(VERSION 3.18.1)

project("fusionnet")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ARM-specific optimizations for mobile performance
if(ANDROID_ABI STREQUAL "armeabi-v7a" OR ANDROID_ABI STREQUAL "arm64-v8a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -ffast-math -ftree-vectorize")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

# Compiler-specific optimizations
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNDEBUG")
    # Enable ARM-specific features
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a")
    endif()
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Add ButterworthFilter library
add_library(
    butterworth_filter
    STATIC
    ButterworthFilter.cpp
    ButterworthFilter.hpp
)

# Set compile options for ButterworthFilter
target_compile_options(butterworth_filter PRIVATE
    -O3
    -ffast-math
    -fno-strict-aliasing
)

# Add native library with JNI
add_library(
    fusionnet
    SHARED
    native-lib.cpp
    ButterworthFilter.cpp
)

# Link libraries
target_link_libraries(
    fusionnet
    # Android logging
    log
    # Android native libraries
    android
    # Math library
    m
)

# Set compile options for main library
target_compile_options(fusionnet PRIVATE
    -O3
    -ffast-math
    -fno-strict-aliasing
    -Wall
    -Wextra
)

# Include directories for fusionnet
target_include_directories(fusionnet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Preprocessor definitions for ARM optimization
target_compile_definitions(fusionnet PRIVATE
    ANDROID_ARM_OPTIMIZATION=1
    FUSIONNET_VERSION_MAJOR=1
    FUSIONNET_VERSION_MINOR=0
    FUSIONNET_VERSION_PATCH=0
)

# Find and link OpenMP if available for parallel processing
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(fusionnet OpenMP::OpenMP_CXX)
    target_compile_definitions(fusionnet PRIVATE USE_OPENMP=1)
endif()

# Android-specific settings for real-time performance
if(ANDROID)
    target_compile_definitions(fusionnet PRIVATE
        ANDROID_PLATFORM=android-${ANDROID_PLATFORM_LEVEL}
        REAL_TIME_PRIORITY=1
    )
endif()

# Set output directory
set_target_properties(fusionnet PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

# Debug configuration
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(fusionnet PRIVATE DEBUG_BUILD=1)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
    message(STATUS "Building FusionNet in Debug mode")
else()
    target_compile_definitions(fusionnet PRIVATE RELEASE_BUILD=1)
    message(STATUS "Building FusionNet in Release mode with ARM optimizations")
endif()

# Print build information
message(STATUS "FusionNet Build Configuration:")
message(STATUS "  Android ABI: ${ANDROID_ABI}")
message(STATUS "  Android Platform: ${ANDROID_PLATFORM}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler Flags: ${CMAKE_CXX_FLAGS}")

# Custom target for running tests (if test files exist)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/")
    add_executable(
        fusionnet_test
        test/test_butterworth.cpp
        ButterworthFilter.cpp
    )
    
    target_link_libraries(fusionnet_test
        m
    )
    
    target_include_directories(fusionnet_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
    message(STATUS "FusionNet test target created")
endif()
