# Set the minimum required version of CMake for this project.
cmake_minimum_required(VERSION 3.13)

# Set the AWS service components used by this project.
set(SERVICE_COMPONENTS s3 sts transfer)

# Set this project's name.
project("awssdk")

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    set(A l)
elseif(CMAKE_SYSTEM_NAME STREQUAL Darwin)
    set(A m)
elseif(CMAKE_SYSTEM_NAME STREQUAL Windows)
    set(A w)
else()
    message(FATAL_ERROR "Unsupported OS")
endif()

# CMAKE_OSX_ARCHITECTURES is only set for explicit/universal macOS builds; otherwise
# the build targets the host, reported in CMAKE_SYSTEM_PROCESSOR.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(TARGET_ARCH MATCHES "aarch64|arm64")
    set(A "${A}a64")
elseif(TARGET_ARCH MATCHES "x86_64|AMD64|x64")
    set(A "${A}i64")
else()
    message(FATAL_ERROR "Unsupported target architecture: ${TARGET_ARCH}")
endif()

set(MY_LIBRARY_NAME "awssdk.${A}")

# Set the C++ standard to use to build this target.
# At least C++ 11 is required for the AWS SDK for C++.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optional: point the build at an SDK that is not in a standard location.
#   cmake -S . -B build -DAWS_SDK_ROOT=/path/to/prefix
set(AWS_SDK_ROOT "" CACHE PATH "AWS SDK for C++ install prefix (optional)")
if (AWS_SDK_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${AWS_SDK_ROOT}")
endif ()

# Use the MSVC variable to determine if this is a Windows build.
set(WINDOWS_BUILD ${MSVC})

if (WINDOWS_BUILD) # Set the location where CMake can find the installed libraries for the AWS SDK.
    string(REPLACE ";" "/aws-cpp-sdk-all;" SYSTEM_MODULE_PATH "${CMAKE_SYSTEM_PREFIX_PATH}/aws-cpp-sdk-all")
    list(APPEND CMAKE_PREFIX_PATH ${SYSTEM_MODULE_PATH})
endif ()

# s3-crt is not built by every SDK distribution, so link it only where it exists.
# Probing with find_package rather than a filesystem test keeps this working across
# install layouts (lib/cmake/... for most, share/... for vcpkg).
find_package(aws-cpp-sdk-s3-crt QUIET CONFIG)
if (aws-cpp-sdk-s3-crt_FOUND)
    list(APPEND SERVICE_COMPONENTS s3-crt)
endif ()

# Find the AWS SDK for C++ package.
find_package(AWSSDK REQUIRED COMPONENTS ${SERVICE_COMPONENTS})

#add_subdirectory(src)
add_library(${MY_LIBRARY_NAME} SHARED
        src/aws_credentials.cpp
        src/aws_session.cpp
        src/module.cpp
        src/s3_client.cpp
        src/utils.cpp
)

target_link_libraries(${MY_LIBRARY_NAME} PUBLIC ${AWSSDK_LINK_LIBRARIES})

if (MSVC)
    set(Q_LIB "${CMAKE_BINARY_DIR}/q.lib")
    file(DOWNLOAD "https://github.com/KxSystems/kdb/raw/master/w64/q.lib"
            "${Q_LIB}" TLS_VERIFY ON STATUS Q_LIB_STATUS)
    list(GET Q_LIB_STATUS 0 Q_LIB_ERROR)
    if (NOT Q_LIB_ERROR EQUAL 0)
        list(GET Q_LIB_STATUS 1 Q_LIB_MESSAGE)
        message(FATAL_ERROR "Failed to download q.lib: ${Q_LIB_MESSAGE}")
    endif ()
    target_link_libraries(${MY_LIBRARY_NAME} PRIVATE "${Q_LIB}")
endif ()

set_target_properties(${MY_LIBRARY_NAME} PROPERTIES PREFIX "")
if (APPLE)
    set_target_properties(${MY_LIBRARY_NAME} PROPERTIES SUFFIX ".so")
    # The kdb+ C API symbols (ktn, kp, ss, dl, ...) are not in any library: they are
    # provided by the q process that loads this module. ld on macOS rejects undefined
    # symbols at link time, so defer their resolution to load time.
    target_link_options(${MY_LIBRARY_NAME} PRIVATE "-Wl,-undefined,dynamic_lookup")
endif ()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${MY_LIBRARY_NAME} PRIVATE
        -g3 -O0 -fprofile-arcs -ftest-coverage -fno-inline
    )

    target_link_options(${MY_LIBRARY_NAME} PRIVATE
        --coverage
    )
endif()

# Installs straight into CMAKE_INSTALL_PREFIX. The destinations are spelled out
# because CMake's defaults would place the library under lib/ instead.
#
# No RPATH is baked into the installed module: the AWS SDK shared libraries are
# located at load time, so callers must have them on LD_LIBRARY_PATH (or
# DYLD_LIBRARY_PATH / PATH) before loading this library.
install(TARGETS ${MY_LIBRARY_NAME}
        LIBRARY DESTINATION .   # .so / .dylib
        RUNTIME DESTINATION .   # .dll, Windows
        ARCHIVE DESTINATION .)  # .lib import library, Windows

#include(CTest)
#if (BUILD_TESTING)
#    add_subdirectory(tests)
#endif ()
