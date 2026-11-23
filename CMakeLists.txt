# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Skipper: the avatar renderer used by the Porthole board (hardware/avatar-render in skipper-ai,
# where this repo is the submodule hardware/skipper-pod/firmware). Override with AVATAR_RENDER_DIR.
# MINIMAL_BUILD means only boards that require it (porthole) compile it.
if(DEFINED ENV{AVATAR_RENDER_DIR})
    set(SKIPPER_AVATAR_RENDER_DIR "$ENV{AVATAR_RENDER_DIR}")
else()
    set(SKIPPER_AVATAR_RENDER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../avatar-render")
endif()
if(EXISTS "${SKIPPER_AVATAR_RENDER_DIR}/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${SKIPPER_AVATAR_RENDER_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.6.0")
project(xiaozhi)
