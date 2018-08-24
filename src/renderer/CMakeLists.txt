find_package(PkgConfig REQUIRED)
pkg_check_modules(BOT2_VIS bot2-vis)
if(NOT BOT2_VIS_FOUND)
    message("\n\n")
    message("bot2-vis not found.  Not building laser renderer")
    message("\n")
    return()
endif()

# Necessary to build on OSX
find_package(OpenGL REQUIRED)

add_definitions(-std=gnu99)

add_library(laser-utils-renderer SHARED
    renderer_laser.c)

target_link_libraries(laser-utils-renderer ${OPENGL_LIBRARIES})

set(REQUIRED_LIBS laser-utils bot2-vis)

pods_use_pkg_config_packages(laser-utils-renderer ${REQUIRED_LIBS})

pods_install_headers(renderer_laser.h DESTINATION laser_utils)
pods_install_libraries(laser-utils-renderer)
pods_install_pkg_config_file(laser-utils-renderer
    CFLAGS
    LIBS -llaser-utils-renderer
    REQUIRES ${REQUIRED_LIBS}
    VERSION 0.0.1)