cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED ENV{OO_PS4_TOOLCHAIN})
    set(OO_PS4_TOOLCHAIN "$ENV{HOME}/OpenOrbis")
else()
    set(OO_PS4_TOOLCHAIN "$ENV{OO_PS4_TOOLCHAIN}")
endif()

# Force Clang compilers
set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "CXX compiler" FORCE)

project(ps4-xcloud C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_SYSROOT ${OO_PS4_TOOLCHAIN})

# Compile flags matching official OpenOrbis toolchain specifications
set(COMMON_FLAGS "--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot ${OO_PS4_TOOLCHAIN} -isystem ${OO_PS4_TOOLCHAIN}/include")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -isystem ${OO_PS4_TOOLCHAIN}/include/c++/v1")

# Linker flags: use -nostdlib to avoid host crtbegin.o/lgcc lookup
set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld -target x86_64-pc-freebsd12-elf -nostdlib -Wl,-pie -Wl,--script,${OO_PS4_TOOLCHAIN}/link.x -Wl,--eh-frame-hdr -L${OO_PS4_TOOLCHAIN}/lib ${OO_PS4_TOOLCHAIN}/lib/crt1.o")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Project Sources
option(XCLOUD_DISPLAY_DIAGNOSTIC "Build the display-only framebuffer diagnostic" OFF)
option(XCLOUD_NETWORK_DIAGNOSTIC "Build the PS4 POSIX network diagnostic" OFF)
option(XCLOUD_WEBRTC_DIAGNOSTIC "Build the PS4 libdatachannel transport diagnostic" OFF)
option(XCLOUD_ENABLE_WEBRTC "Build with pinned libdatachannel WebRTC dependencies" OFF)
set(XCLOUD_CLIENT_ID "1f907974-e22b-4810-a9de-d9647380c97e")

if((XCLOUD_DISPLAY_DIAGNOSTIC AND XCLOUD_NETWORK_DIAGNOSTIC)
    OR (XCLOUD_DISPLAY_DIAGNOSTIC AND XCLOUD_WEBRTC_DIAGNOSTIC)
    OR (XCLOUD_NETWORK_DIAGNOSTIC AND XCLOUD_WEBRTC_DIAGNOSTIC))
    message(FATAL_ERROR "Select at most one diagnostic build mode.")
endif()

if(XCLOUD_WEBRTC_DIAGNOSTIC)
    set(XCLOUD_ENABLE_WEBRTC ON)
endif()

if(XCLOUD_ENABLE_WEBRTC)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_HAVE_LIBC_PTHREAD TRUE)
    set(CMAKE_THREAD_LIBS_INIT "")

    foreach(required_path
        "third_party/libdatachannel/CMakeLists.txt"
        "third_party/mbedtls/CMakeLists.txt"
        "third_party/libdatachannel/deps/libjuice/src/udp.c"
        "third_party/libdatachannel/deps/usrsctp/CMakeLists.txt"
    )
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required_path}")
            message(FATAL_ERROR
                "WebRTC dependencies are missing (${required_path}). "
                "Initialize submodules recursively before configuring XCLOUD_ENABLE_WEBRTC.")
        endif()
    endforeach()

    include(cmake/ApplyOrbisPatches.cmake)
    apply_orbis_patch(
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/libdatachannel-orbis.patch"
    )
    apply_orbis_patch(
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel/deps/libjuice"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/libjuice-orbis.patch"
    )
    apply_orbis_patch(
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel/deps/usrsctp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/usrsctp-orbis.patch"
    )
    apply_orbis_patch(
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/mbedtls-orbis.patch"
    )

    add_compile_definitions(__ORBIS__ _BSD_SOURCE)
    add_compile_definitions(
        MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_orbis_config.h\"
    )

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static WebRTC dependencies" FORCE)
    set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "Build static WebRTC dependencies" FORCE)
    set(USE_MBEDTLS ON CACHE BOOL "Use Mbed TLS for WebRTC cryptography" FORCE)
    set(ENABLE_MBEDTLS ON CACHE BOOL "Use Mbed TLS for SRTP" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "Disable dependency examples" FORCE)
    set(NO_TESTS ON CACHE BOOL "Disable dependency tests" FORCE)
    set(RTC_UPDATE_VERSION_HEADER OFF CACHE BOOL "Keep pinned headers unchanged" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "Disable Mbed TLS programs" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "Disable Mbed TLS tests" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Allow upstream compiler warnings" FORCE)
    set(MBEDTLS_USER_CONFIG_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_orbis_config.h"
        CACHE FILEPATH "Mbed TLS configuration additions for Orbis" FORCE
    )
    set(sctp_build_shared_lib OFF CACHE BOOL "Build static usrsctp" FORCE)
    set(sctp_build_programs OFF CACHE BOOL "Disable usrsctp programs" FORCE)
    set(sctp_debug OFF CACHE BOOL "Disable usrsctp debug output" FORCE)
    set(sctp_invariants OFF CACHE BOOL "Disable usrsctp invariants" FORCE)
    set(sctp_werror OFF CACHE BOOL "Do not treat dependency warnings as errors" FORCE)

    add_subdirectory(third_party/mbedtls "${CMAKE_BINARY_DIR}/third_party/mbedtls" EXCLUDE_FROM_ALL)
    add_library(MbedTLS::MbedTLS STATIC IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES
        IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedtls.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include"
    )
    add_library(MbedTLS::MbedCrypto STATIC IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedCrypto PROPERTIES
        IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include"
    )
    add_library(MbedTLS::MbedX509 STATIC IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedX509 PROPERTIES
        IMPORTED_LOCATION "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedx509.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include"
        INTERFACE_LINK_LIBRARIES MbedTLS::MbedCrypto
    )
    set(MbedTLS_FOUND TRUE)
    set(MbedTLS_VERSION "3.6.2")
    set(MbedTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include")
    set(MbedTLS_LIBRARY "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedtls.a")
    set(MbedCrypto_LIBRARY "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedcrypto.a")
    set(MbedX509_LIBRARY "${CMAKE_BINARY_DIR}/third_party/mbedtls/library/libmbedx509.a")

    add_subdirectory(
        third_party/libdatachannel
        "${CMAKE_BINARY_DIR}/third_party/libdatachannel"
        EXCLUDE_FROM_ALL
    )
    add_dependencies(datachannel-static mbedtls mbedx509 mbedcrypto)
endif()

if(XCLOUD_DISPLAY_DIAGNOSTIC OR XCLOUD_NETWORK_DIAGNOSTIC OR XCLOUD_WEBRTC_DIAGNOSTIC)
    set(SOURCES
        "src/display.cpp"
        "src/frame_renderer.cpp"
        "src/main.cpp"
    )
    if(XCLOUD_NETWORK_DIAGNOSTIC)
        list(APPEND SOURCES "src/network_probe.cpp")
    elseif(XCLOUD_WEBRTC_DIAGNOSTIC)
        list(APPEND SOURCES "src/webrtc_probe.cpp")
    endif()
else()
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "src/*.cpp"
        "src/*.c"
    )
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/network_probe.cpp")
    if(NOT XCLOUD_ENABLE_WEBRTC)
        list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc_probe.cpp")
    endif()
endif()

add_executable(ps4_xcloud.elf ${SOURCES})
target_compile_definitions(ps4_xcloud.elf PRIVATE __ORBIS__)
if(XCLOUD_DISPLAY_DIAGNOSTIC)
    target_compile_definitions(ps4_xcloud.elf PRIVATE XCLOUD_DISPLAY_PROBE)
elseif(XCLOUD_NETWORK_DIAGNOSTIC)
    target_compile_definitions(ps4_xcloud.elf PRIVATE XCLOUD_NETWORK_PROBE)
elseif(XCLOUD_WEBRTC_DIAGNOSTIC)
    target_compile_definitions(ps4_xcloud.elf PRIVATE XCLOUD_WEBRTC_PROBE)
else()
    target_compile_definitions(ps4_xcloud.elf PRIVATE "XCLOUD_CLIENT_ID=\"${XCLOUD_CLIENT_ID}\"")
endif()

# Target libraries
target_link_libraries(ps4_xcloud.elf
    c
    kernel
    c++
    SceVideoOut
    SceAudioOut
    SceVideodec
    ScePad
    SceNet
    SceNetCtl
    SceHttp
    SceSsl
    SceSysmodule
    SceUserService
)
if(XCLOUD_ENABLE_WEBRTC)
    target_compile_definitions(ps4_xcloud.elf PRIVATE XCLOUD_ENABLE_WEBRTC)
    target_link_libraries(ps4_xcloud.elf datachannel-static)
endif()

# Post-build: generate signed eboot.bin
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pkg_root")
add_custom_command(TARGET ps4_xcloud.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN}"
        ${OO_PS4_TOOLCHAIN}/bin/linux/create-fself
        -in ps4_xcloud.elf
        -out ps4_xcloud.oelf
        --eboot "${CMAKE_CURRENT_BINARY_DIR}/pkg_root/eboot.bin"
        --paid 0x3800000000000011
    COMMENT "Signing ps4_xcloud.elf -> pkg_root/eboot.bin"
)
