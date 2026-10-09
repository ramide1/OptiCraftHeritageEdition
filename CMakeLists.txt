cmake_minimum_required(VERSION 3.21)

project(OptiCraftHeritage
    VERSION 1.7.3
    DESCRIPTION "OptiCraft Heritage - heavily modified clean-room C++ implementation"
    LANGUAGES C CXX
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SourceSelection.cmake)

# StrictMathCompat reproduces the operation ordering of Java StrictMath/fdlibm.
# In particular on Broadway/PowerPC, contracting a multiply+add into FMA can
# change the last bit. Keep contraction disabled for this TU only so the rest
# of the renderer/world code keeps its normal optimization profile.
file(GLOB FDLIBM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/java/fdlibm/*.c"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/java/StrictMathCompat.cpp"
        ${FDLIBM_SOURCES}
        # -fno-strict-aliasing: fdlibm's __HI/__LO macros read and write the
        # halves of a double through int* type-punning, which strict aliasing
        # is allowed to miscompile -- on armv6k/-O3 the 3DS toolchain turned
        # __ieee754_sqrt(0.93) into -0.0, which made MathHelper::sqrt_float
        # return zero for every positive argument and moveFlying's
        # "if (f3 < 0.01f) return" eat all grounded movement (the 3DS
        # walk report). Every consumer of sqrt/atan2/log/pow on the console
        # goes through these TUs, so the flag rides with -ffp-contract=off.
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-strict-aliasing"
    )
elseif(MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/java/StrictMathCompat.cpp"
        ${FDLIBM_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/fp:strict"
    )
endif()

# PlayStation 2 build. The EE toolchain (psdevwindows/ps2sdk/ps2dev.cmake, wired
# in by the ps2-* presets) defines PS2; -DPLATFORM=PS2 works too. All PS2 logic
# lives in cmake/ps2.cmake, which builds its own target and return()s so none of
# the desktop SDL2/glad/OpenGL configuration below runs.
if(PS2 OR (DEFINED PLATFORM AND PLATFORM STREQUAL "PS2"))
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps2.cmake)
    return()
endif()

# Nintendo Wii build. cmake/wii_toolchain.cmake (wired in by the wii-* presets)
# defines NINTENDO_WII; -DPLATFORM=WII works too. Same shape as the PS2 branch:
# cmake/wii.cmake builds its own target and return()s, so none of the desktop
# SDL2/glad/OpenGL configuration below runs.
if(NINTENDO_WII OR (DEFINED PLATFORM AND PLATFORM STREQUAL "WII"))
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/wii.cmake)
    return()
endif()

enable_testing()

# Nintendo 3DS build. cmake/3ds_toolchain.cmake (wired in by the 3ds-* presets)
# defines NINTENDO_3DS; -DPLATFORM=3DS works too. Same shape as the Wii branch:
# cmake/3ds.cmake builds its own target and return()s, so none of the desktop
# SDL2/glad/OpenGL configuration below runs.
if(NINTENDO_3DS OR (DEFINED PLATFORM AND PLATFORM STREQUAL "3DS"))
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/3ds.cmake)
    return()
endif()

function(opticraft_add_optional_test target test_source)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_source}")
        message(STATUS "${target} skipped: ${test_source} is not available in this source package")
        return()
    endif()

    add_executable(${target}
        "${test_source}"
        ${ARGN}
    )
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME ${target} COMMAND ${target})
endfunction()

opticraft_add_optional_test(IntCacheTests
    tests/IntCacheTests.cpp
    src/net/minecraft/src/IntCache.cpp
)

opticraft_add_optional_test(Ps2MatrixStackTests
    tests/Ps2MatrixStackTests.cpp
    src/ps2/render/Ps2MatrixStack.cpp
)
if(TARGET Ps2MatrixStackTests)
    target_compile_definitions(Ps2MatrixStackTests PRIVATE PS2_PLATFORM=1 MC_LOG_LEVEL=0)
endif()

opticraft_add_optional_test(MathHelperFloorTests
    tests/MathHelperFloorTests.cpp
)

opticraft_add_optional_test(StructureReservationTests
    tests/StructureReservationTests.cpp
    src/net/minecraft/src/StructureBoundingBox.cpp
)

opticraft_add_optional_test(WiiContainerCursorPolicyTests
    tests/WiiContainerCursorPolicyTests.cpp
)

opticraft_add_optional_test(Ps2LightFactorCacheTests
    tests/Ps2LightFactorCacheTests.cpp
    src/ps2/render/Ps2RenderLighting.cpp
    src/ps2/render/Ps2MatrixStack.cpp
)
if(TARGET Ps2LightFactorCacheTests)
    target_compile_definitions(Ps2LightFactorCacheTests PRIVATE PS2_PLATFORM=1 MC_LOG_LEVEL=0)
endif()

opticraft_add_optional_test(WorldClientEntityIdentityTests
    tests/WorldClientEntityIdentityTests.cpp
)

opticraft_add_optional_test(JavaHashSetTests
    tests/JavaHashSetTests.cpp
    src/net/minecraft/src/ChunkCoordIntPair.cpp
)
if(TARGET JavaHashSetTests AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(JavaHashSetTests PRIVATE -std=c++17)
endif()

opticraft_add_optional_test(LocalStatsFormatTests
    tests/LocalStatsFormatTests.cpp
    src/net/minecraft/src/LocalStatsFormat.cpp
)

opticraft_add_optional_test(PcLegacyBlockCollisionMathTests
    tests/PcLegacyBlockCollisionMathTests.cpp
)

opticraft_add_optional_test(EndDragonSpawnPolicyTests
    tests/EndDragonSpawnPolicyTests.cpp
)

opticraft_add_optional_test(ChunkMemoryPolicyTests
    tests/ChunkMemoryPolicyTests.cpp
)
if(TARGET ChunkMemoryPolicyTests)
    target_compile_definitions(ChunkMemoryPolicyTests PRIVATE PS2_PLATFORM=1 MC_LOG_LEVEL=0)
endif()

opticraft_add_optional_test(Ps2MeshMemoryPolicyTests
    tests/Ps2MeshMemoryPolicyTests.cpp
)
if(TARGET Ps2MeshMemoryPolicyTests)
    target_compile_definitions(Ps2MeshMemoryPolicyTests PRIVATE PS2_PLATFORM=1 MC_LOG_LEVEL=0)
endif()

opticraft_add_optional_test(Ps2FramePacingPolicyTests
    tests/Ps2FramePacingPolicyTests.cpp
)
if(TARGET Ps2FramePacingPolicyTests)
    target_compile_definitions(Ps2FramePacingPolicyTests PRIVATE PS2_PLATFORM=1 MC_LOG_LEVEL=0)
endif()

# Keep third-party projects small: OptiCraft only needs their static libraries.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib examples" FORCE)

add_subdirectory(external/glad EXCLUDE_FROM_ALL)
add_subdirectory(external/zlib EXCLUDE_FROM_ALL)

# SDL3 (external/SDL3 submodule, zlib license): gamepad input ONLY -- etapa 3
# of the 2026-10 SDL->GLFW/OpenAL/SDL3 migration. Window, GL context and
# keyboard/mouse stay on GLFW, audio stays on OpenAL, and desktop sockets
# are native (Winsock/BSD, like the console backends) -- no SDL_net of any
# generation remains. InputBackend_PC is the only translation unit that
# touches SDL3, and it initialises SDL_INIT_GAMEPAD alone. Static build, no
# tests/examples/install rules.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL3 EXCLUDE_FROM_ALL)

# GLFW (external/glfw submodule, zlib license): window + GL context +
# keyboard/mouse for the desktop target -- etapa 1 of the 2026-10
# SDL->GLFW/OpenAL/SDL3 migration. No SDL_net of any generation remains
# (desktop sockets are native, like the console backends); audio left SDL
# in etapa 2 (OpenAL-soft below). The minimum GL profile stays 1.3, as it
# has effectively been all along.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw EXCLUDE_FROM_ALL)

# OpenAL-soft (external/openal-soft submodule, LGPL-2.0-or-later): the desktop
# audio output -- etapa 2 of the 2026-10 SDL->GLFW/OpenAL/SDL3 migration.
# SoundManager_PC keeps its own software mixer (same buses/attenuation math
# as the console backends) and pushes the mixed stream through one flat
# OpenAL source, so no AL 3D positioning is involved. Static build, no
# utils/examples/tests/install rules; ALSOFT_UPDATE_BUILD_VERSION stays off
# so the build never shells out to git. The SDL2/SDL3 backends are already
# OFF in openal-soft's own defaults, keeping the old audio device out of
# the new output path.
# NOTE: LIBTYPE is a plain (non-cache) variable on purpose so it does not
# leak into the cache for any other subproject.
set(LIBTYPE STATIC)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_UPDATE_BUILD_VERSION OFF CACHE BOOL "" FORCE)
# No C++20 modules: GCC's module mapper conflicts with the x86 intrinsics
# headers (conflicting language linkage on _mm_*), breaking the build with
# recent MinGW. The classic-header path is fully supported upstream.
set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/openal-soft EXCLUDE_FROM_ALL)

# --- Microsoft account login ------------------------------------------------
# The flow itself is common code (src/net/minecraft/src/MicrosoftAuthFlow.cpp
# through the PlatformAuth family, see cmake/SourceSelection.cmake); this
# block wires the desktop transport in every build: Mbed TLS
# (external/mbedtls, Apache-2.0, pinned submodule -- see .gitmodules) plus
# the Mozilla CA bundle (resources/cacert.pem, MPL-2.0) compiled in through
# the template below. The client id stays a build input because Microsoft
# does not register new third-party applications for the Minecraft scopes
# any more -- new Azure registrations answer 403 Invalid app registration at
# Minecraft Services, so the default is PrismLauncher's public client id
# (GPL-3.0, from their CMakeLists.txt). By building with this default you
# accept the Microsoft Identity Platform Terms of Use, the same condition
# Prism ships its key under; pass -DOPTICRAFT_MSA_CLIENT_ID=<id> for your
# own allowlisted id, or an empty string to leave the login UI hidden (the
# transport still compiles in).
set(OPTICRAFT_MSA_CLIENT_ID "c36a9fb6-4f2a-41ff-90bd-ae7cc92031eb" CACHE STRING "Microsoft identity platform client id for the login flow (empty = login unavailable)")
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/mbedtls EXCLUDE_FROM_ALL)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/cacert.pem" OPTICRAFT_CA_PEM)
set(OPTICRAFT_CA_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/PcCaBundle.cpp.in"
               "${OPTICRAFT_CA_BUNDLE_DIR}/PcCaBundle.cpp" @ONLY)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Build the desktop target from common code plus the PC tree. Foreign console
# sources are never discovered, so adding a new Wii/PS2 .cpp cannot silently
# enter a desktop link.
opticraft_collect_platform_sources(OPTICRAFT_COMMON_SOURCES pc)
set(OPTICRAFT_MINIZIP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c"
)
file(GLOB_RECURSE OPTICRAFT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
)

if(WIN32)
    opticraft_exclude_sources(OPTICRAFT_COMMON_SOURCES "[/\\]pc[/\\]linux[/\\]")
elseif(UNIX)
    # macOS lands here too (APPLE implies UNIX): src/pc/linux is the shared
    # POSIX implementation (File/Resource/Runtime carry their own __APPLE__
    # paths), only the win32 variants are foreign.
    opticraft_exclude_sources(OPTICRAFT_COMMON_SOURCES "[/\\]pc[/\\]win32[/\\]")
else()
    message(FATAL_ERROR "OptiCraft currently supports Windows, Linux and macOS only")
endif()

# Each public platform facade has exactly one implementation in this target.
opticraft_select_platform_backends(OPTICRAFT_COMMON_SOURCES PC PC PC)

# Unified diagnostic verbosity, the same knob cmake/ps2.cmake and cmake/wii.cmake
# expose. The desktop target left it undefined, so platform/Log.h fell back to 0
# and every MC_LOG_* call compiled to nothing -- including the HeapValidate
# checkpoints in Minecraft.cpp, which then reported through a macro that no
# longer existed. Level 2 is needed for the MC_LOG_DEBUG lines around the
# deferred GUI/World destruction.
set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

# Dedicated low-end desktop profile. The public entry point is the
# gcc32-legacy-release preset; this option is the internal architecture switch
# consumed by the platform tuning layer. Keep it separate from console-low: the
# Legacy PC build must not inherit world-generation or bounded-world policies.
option(PC_LEGACY_BUILD "Enable the 32-bit low-end PC renderer/profile" OFF)
if(PC_LEGACY_BUILD AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "PC_LEGACY_BUILD requires a 32-bit toolchain; use the gcc32-legacy-release preset")
endif()

add_executable(OptiCraft
    ${OPTICRAFT_COMMON_SOURCES}
    ${OPTICRAFT_MINIZIP_SOURCES}
    ${OPTICRAFT_HEADERS}
)

target_compile_features(OptiCraft PRIVATE cxx_std_17)
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    # A game, not a console tool: link the Windows GUI subsystem so no
    # console window opens next to the game window. Logs still go to the
    # log file, and the crash handler shows its own dialog.
    # MinGW keeps plain main() under -mwindows; MSVC needs its entry
    # redirected the same way (entry_point option below).
    WIN32_EXECUTABLE TRUE
)

# A level, not a boolean, so it is passed through as-is rather than through
# $<BOOL:>, which would collapse 2 to 1.
target_compile_definitions(OptiCraft PRIVATE MC_LOG_LEVEL=${MC_LOG_LEVEL})
# glad is the desktop GL loader: GLFW must never pull the system GL headers
# itself, or glad errors out ("OpenGL header already included") in every TU
# that reaches glfw3.h before glad.h (e.g. via lwjgl/GLContext.h). Canonical
# fix per GLFW docs; GL users include glad directly.
target_compile_definitions(OptiCraft PRIVATE GLFW_INCLUDE_NONE)
if(PC_LEGACY_BUILD)
    target_compile_definitions(OptiCraft PRIVATE PC_LEGACY_BUILD=1)
endif()

# Link-Time Optimization for Release. The port is built from hundreds of tiny
# cross-translation-unit accessors (getBlockId, getSavedLightValue, Chunk /
# NibbleArray getters) called millions of times per frame in the hot loops;
# without LTO the compiler cannot inline them across .cpp boundaries.
#
# OFF by default: MinGW's bundled ld can fail the final link with
# "plugin needed to handle lto object" because it isn't invoked with GCC's LTO
# linker plugin. The bulk of the codegen win already comes from -O2/-O3 (per-TU
# inlining); LTO only adds the cross-TU part. Turn this on (and make sure the
# toolchain uses gcc-ar / gcc-ranlib and -fuse-linker-plugin) to chase the rest.
option(OPTICRAFT_ENABLE_LTO "Enable Link-Time Optimization for Release builds" OFF)
if(OPTICRAFT_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES CXX)
    if(ipo_supported)
        set_target_properties(OptiCraft PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "IPO/LTO requested but not supported by this toolchain, skipping: ${ipo_error}")
    endif()
endif()

target_include_directories(OptiCraft PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib/contrib/minizip"
)

target_link_libraries(OptiCraft PRIVATE
    glad
    glfw
    OpenAL::OpenAL
    SDL3::SDL3
    zlibstatic
    OpenGL::GL
    Threads::Threads
)

# Microsoft login: attach the vendored TLS transport to the desktop target.
# mbedcrypto/mbedx509/mbedtls are the in-tree static libraries from
# external/mbedtls; ws2_32 is linked unconditionally on Windows already
# (native game sockets are Winsock directly), so this only adds the TLS
# libraries themselves, in every build.
target_sources(OptiCraft PRIVATE "${OPTICRAFT_CA_BUNDLE_DIR}/PcCaBundle.cpp")
target_compile_definitions(OptiCraft PRIVATE
    PLATFORM_MSAUTH=1
    OPTICRAFT_MSA_CLIENT_ID="${OPTICRAFT_MSA_CLIENT_ID}"
)
target_link_libraries(OptiCraft PRIVATE mbedtls mbedx509 mbedcrypto)

if(WIN32)
    target_compile_definitions(OptiCraft PRIVATE
        MC_WIN32
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(OptiCraft PRIVATE shell32 psapi d3d9 ws2_32)

    if(MSVC)
        target_compile_options(OptiCraft PRIVATE /MP /bigobj /utf-8)
        # WIN32_EXECUTABLE above makes CMake pass /SUBSYSTEM:WINDOWS, whose
        # default entry (WinMainCRTStartup) does not exist here: the game
        # keeps plain main() on every compiler, so redirect it explicitly.
        target_link_options(OptiCraft PRIVATE /ENTRY:mainCRTStartup)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(OptiCraft PRIVATE
            -Wa,-mbig-obj
            $<$<CONFIG:Release>:-Os>
            $<$<NOT:$<CONFIG:Release>>:-g>
        )
        if(PC_LEGACY_BUILD)
            # Tune scheduling for Bonnell without raising the inherited SSE2 ISA baseline.
            target_compile_options(OptiCraft PRIVATE
                $<$<CONFIG:Release>:-O2>
                -mtune=bonnell
                $<$<CONFIG:Release>:-fomit-frame-pointer>
                $<$<NOT:$<CONFIG:Release>>:-fno-omit-frame-pointer>
                -fno-math-errno
                -fno-trapping-math
            )
        else()
            target_compile_options(OptiCraft PRIVATE
                -fno-omit-frame-pointer
            )
        endif()
        target_link_options(OptiCraft PRIVATE
            -static
            -fno-ident
            -fno-asynchronous-unwind-tables
            -fvisibility=hidden
            $<$<CONFIG:Release>:-s>
        )
    endif()
elseif(APPLE)
    # Same shape as MC_WIN32/MC_LINUX: an accurate platform macro instead of
    # telling Darwin code it is running on Linux. Nothing reads it today.
    target_compile_definitions(OptiCraft PRIVATE MC_MACOS)
elseif(UNIX)
    target_compile_definitions(OptiCraft PRIVATE MC_LINUX)
endif()

# Put every configuration in a predictable folder instead of beside CMake's
# generated files. Explicit per-config properties avoid an extra Debug/Debug.
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(OptiCraft PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/${config}"
    )
endforeach()

if(PC_LEGACY_BUILD)
    set_target_properties(OptiCraft PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/gcc32-legacy-release"
    )
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT OptiCraft)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files"
    FILES ${OPTICRAFT_COMMON_SOURCES} ${OPTICRAFT_HEADERS}
)
source_group("Third Party\\minizip" FILES ${OPTICRAFT_MINIZIP_SOURCES})
