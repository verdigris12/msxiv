cmake_minimum_required(VERSION 3.10)
project(msxiv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic")

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IMAGEMAGICK REQUIRED MagickWand)

add_executable(msxiv
    src/main.c
    src/viewer.c
    src/viewer.h
    src/config.c
    src/config.h
    src/commands.c
    src/commands.h
)

target_include_directories(msxiv PRIVATE
    ${X11_INCLUDE_DIR}
    ${IMAGEMAGICK_INCLUDE_DIRS}
)

# IMPORTANT: Pass the ImageMagick compiler flags (which define MAGICKCORE_HDRI_ENABLE, etc.).
target_compile_options(msxiv PRIVATE
    ${IMAGEMAGICK_CFLAGS_OTHER}
)

target_link_libraries(msxiv
    ${X11_LIBRARIES}
    ${IMAGEMAGICK_LIBRARIES}
)

install(TARGETS msxiv RUNTIME DESTINATION bin)
