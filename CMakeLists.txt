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
set(XCLOUD_CLIENT_ID "" CACHE STRING "Microsoft Entra app registration client ID")

if(XCLOUD_DISPLAY_DIAGNOSTIC)
    set(SOURCES
        "src/display.cpp"
        "src/main.cpp"
    )
else()
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "src/*.cpp"
        "src/*.c"
    )
endif()

add_executable(ps4_xcloud.elf ${SOURCES})
target_compile_definitions(ps4_xcloud.elf PRIVATE __ORBIS__)
if(XCLOUD_DISPLAY_DIAGNOSTIC)
    target_compile_definitions(ps4_xcloud.elf PRIVATE XCLOUD_DISPLAY_PROBE)
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

# Post-build: generate signed eboot.bin
add_custom_command(TARGET ps4_xcloud.elf POST_BUILD
    COMMAND ${OO_PS4_TOOLCHAIN}/bin/linux/create-fself
        -in ps4_xcloud.elf
        -out ps4_xcloud.oelf
        --eboot "${CMAKE_CURRENT_SOURCE_DIR}/pkg_root/eboot.bin"
        --paid 0x3800000000000011
    COMMENT "Signing ps4_xcloud.elf -> pkg_root/eboot.bin"
)
