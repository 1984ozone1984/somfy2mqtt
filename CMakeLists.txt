cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components)

# Firmware version reported by esp_app_desc and shown on the status page.
#
# ESP-IDF would otherwise derive this from `git describe --dirty`, which appends
# "-dirty" whenever the tree has uncommitted changes — noise on a status page.
# Keep the short hash, since being able to tell which build is running has been
# genuinely useful, but drop the suffix.
#
# CMake caches this, so committing would otherwise leave a stale hash baked in;
# depending on .git/HEAD and .git/index forces a reconfigure when either moves.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD"
    "${CMAKE_CURRENT_LIST_DIR}/.git/index")

execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(GIT_REV)
    set(PROJECT_VER "1.0.0-${GIT_REV}")
else()
    set(PROJECT_VER "1.0.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(somfy2mqtt)
