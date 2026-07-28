cmake_minimum_required(VERSION 3.20)
project(LofiFxPlanarStabilizer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Get OpenFX headers directly without building examples
include(FetchContent)
FetchContent_Declare(openfx
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
    GIT_TAG OFX_Release_1.5.1
    SOURCE_SUBDIR include)
FetchContent_MakeAvailable(openfx)
set(OFX_INCLUDE_DIR "${openfx_SOURCE_DIR}/include")

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
endif()

set(PLUGIN_SOURCES src/PlanarStabilizerPlugin.cpp)
if(APPLE)
    list(APPEND PLUGIN_SOURCES src/MetalWarp.mm)
endif()

add_library(LofiFxPlanarStabilizer MODULE ${PLUGIN_SOURCES})
target_include_directories(LofiFxPlanarStabilizer PRIVATE ${OFX_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(LofiFxPlanarStabilizer PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    CXX_VISIBILITY_PRESET default C_VISIBILITY_PRESET default
)

if(APPLE)
    target_link_libraries(LofiFxPlanarStabilizer "-framework Metal" "-framework Foundation" "-framework Accelerate" "-framework CoreFoundation")
endif()

# Bundle
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/LofiFxPlanarStabilizer.ofx.bundle")
if(APPLE)
    set(BUNDLE_BIN_DIR "${BUNDLE_DIR}/Contents/MacOS")
else()
    set(BUNDLE_BIN_DIR "${BUNDLE_DIR}/Contents/Linux-x86-64")
endif()
set(BUNDLE_RES_DIR "${BUNDLE_DIR}/Contents/Resources")

add_custom_command(TARGET LofiFxPlanarStabilizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_RES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:LofiFxPlanarStabilizer>" "${BUNDLE_BIN_DIR}/LofiFxPlanarStabilizer.ofx"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist" "${BUNDLE_DIR}/Contents/Info.plist"
    COMMENT "Assembling bundle"
)

if(APPLE)
    set(OFX_PLUGIN_INSTALL_DIR "/Library/OFX/Plugins")
else()
    set(OFX_PLUGIN_INSTALL_DIR "/usr/OFX/Plugins")
endif()

install(DIRECTORY "${BUNDLE_DIR}" DESTINATION "${OFX_PLUGIN_INSTALL_DIR}" USE_SOURCE_PERMISSIONS)
add_custom_target(install_local
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${OFX_PLUGIN_INSTALL_DIR}/LofiFxPlanarStabilizer.ofx.bundle"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLE_DIR}" "${OFX_PLUGIN_INSTALL_DIR}/LofiFxPlanarStabilizer.ofx.bundle"
    DEPENDS LofiFxPlanarStabilizer)
