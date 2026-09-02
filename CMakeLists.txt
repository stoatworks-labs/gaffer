cmake_minimum_required(VERSION 3.15)

project(Gaffer
    VERSION 0.1.2
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal build by default so the plugin loads in both native Apple Silicon
# and Intel builds of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a faster single-arch dev build.
#
# This must sit immediately after project() and before any target is defined.
# CMake latches the architecture list when the first target is created, and
# setting it later is silently ignored -- you get an arm64-only binary that the
# build log calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

option(GAFFER_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group,
# and the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX
# plugin alone, with no GL loader anywhere in the configure -- which is what
# the Linux job does, because the FFGL SDK has no Linux path, and GLEW would
# otherwise have to be installed to build something that never calls it.
option(GAFFER_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

if(GAFFER_BUILD_FFGL)
# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule under external/.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. CFFGLPluginInfo registers itself from a
# file-scope constructor in Gaffer.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(gaffer_core OBJECT
    source/Audio.h
    source/Audio.cpp
    source/Clock.h
    source/Clock.cpp
    source/Controls.h
    source/Controls.cpp
    source/Diag.h
    source/Diag.cpp
    source/Lens.h
    source/Lens.cpp
    source/Rack.h
    source/Rack.cpp
    source/Rattle.h
    source/Rattle.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Gaffer.h
    source/Gaffer.cpp
)

target_include_directories(gaffer_core PUBLIC source)
target_link_libraries(gaffer_core PUBLIC ffgl::sdk)
target_compile_definitions(gaffer_core PUBLIC GAFFER_VERSION="${PROJECT_VERSION}")
set_target_properties(gaffer_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too for the include path (and for the
# GLEW_STATIC define the static vcpkg triplet propagates). macOS uses the system
# OpenGL framework and needs none of this.
#
# GLEW arrives through vcpkg.json, which nothing in this file mentions -- so a
# missing manifest fails at CONFIGURE time on Windows only, while every macOS
# build and every macOS CI job stays green. Do not delete vcpkg.json.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(gaffer_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin itself: a loadable macOS .bundle (MODULE library).
# ---------------------------------------------------------------------------
add_library(Gaffer MODULE source/PluginEntry.cpp)
target_link_libraries(Gaffer PRIVATE gaffer_core)

if(APPLE)
    set_target_properties(Gaffer PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Gaffer"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Gaffer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.gaffer"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(Gaffer PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# Offline harness.
#
# Half of this plugin is a number per frame rather than a picture -- where the
# focal plane is, how far the rig has been knocked -- and that half is testable
# without a GPU at all. The other half is a resampler whose kernel is literally
# visible: render a point of light and you are looking at the bokeh. So this is
# not a previewer with some assertions bolted on; it is where the lens, the
# focus puller and the rig are actually checked. See tools/gftest.
# ---------------------------------------------------------------------------
if(GAFFER_BUILD_TOOLS AND APPLE)
    add_executable(gftest tools/gftest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(gftest PRIVATE gaffer_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

endif()  # GAFFER_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build of the same effect, for Resolve/Nuke/Natron/Vegas. CPU
# render against the vendored OFX SDK subset under external/openfx (C headers
# + the official C++ Support library, BSD-3 -- see external/openfx/LICENSE.md).
#
# Links Lens.cpp, Rack.cpp and Rattle.cpp straight from source: the model has
# one home. It does NOT link Gaffer.cpp, which is FFGL glue.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(GafferOFX MODULE
        source/ofx/GafferOFX.cpp
        source/Lens.cpp
        source/Rack.cpp
        source/Rattle.cpp
        source/Controls.cpp
        source/Audio.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(GafferOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(GafferOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports
    # -- that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(GafferOFX PRIVATE Threads::Threads)

    # An OFX plugin is not a macOS .bundle in the CMake sense: hosts expect the
    # fixed layout <Name>.ofx.bundle/Contents/<arch>/<Name>.ofx, assembled here
    # by hand. "MacOS" is the OFX architecture name for universal binaries.
    #
    # Both of these feed cmake/InfoOFX.plist.in, which is parameterised for a
    # reason spelled out at the top of that file: the hardcoded version of it
    # fails nothing until codesign, in the release job, after the tag.
    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Gaffer.ofx.bundle")
    set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.gaffer.ofx")
    set_target_properties(GafferOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Gaffer"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(GafferOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(GafferOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

if(GAFFER_BUILD_FFGL)
install(TARGETS Gaffer
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
endif()  # GAFFER_BUILD_FFGL
