cmake_minimum_required(VERSION 3.16)
project(vplayer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# 所有依赖统一放在项目内 dev/ 目录
set(DEV_DIR "${CMAKE_SOURCE_DIR}/dev" CACHE PATH "依赖根目录")

# FFmpeg 9.0.1
set(FFMPEG_ROOT "${DEV_DIR}/ffmpeg")
set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
set(FFMPEG_LIBS
  "${FFMPEG_ROOT}/lib/avformat.lib"
  "${FFMPEG_ROOT}/lib/avcodec.lib"
  "${FFMPEG_ROOT}/lib/avutil.lib"
  "${FFMPEG_ROOT}/lib/swresample.lib"
  "${FFMPEG_ROOT}/lib/swscale.lib")

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Sonic 库
enable_language(C)
add_library(sonic STATIC "${DEV_DIR}/sonic/sonic.c")
target_include_directories(sonic PUBLIC "${DEV_DIR}/sonic")

add_executable(vplayer ${PLAYER_SOURCES})

target_include_directories(vplayer PRIVATE
  ${SDL2_INCLUDE_DIRS}
  "${DEV_DIR}/sdl2_image/x86_64-w64-mingw32/include/SDL2"
  "${DEV_DIR}/sdl2_ttf/include/SDL2/SDL2"
  ${FFMPEG_INCLUDE_DIRS}
  "${DEV_DIR}/sonic"
  ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(vplayer PRIVATE SDL_MAIN_HANDLED)

# 过滤 SDL2main 相关标志
set(SDL2_LINK "")
foreach(f ${SDL2_LDFLAGS})
  if(NOT f MATCHES "^-lSDL2main$|^-lmingw32$|^-mwindows$")
    list(APPEND SDL2_LINK ${f})
  endif()
endforeach()
foreach(f ${SDL2_CFLAGS_OTHER})
  if(NOT f MATCHES "^-Dmain=")
    target_compile_options(vplayer PRIVATE ${f})
  endif()
endforeach()

target_link_libraries(vplayer PRIVATE ${SDL2_LINK} ${FFMPEG_LIBS} sonic shell32 dwmapi
  "${DEV_DIR}/sdl2_image/x86_64-w64-mingw32/lib/libSDL2_image.dll.a"
  "${DEV_DIR}/sdl2_ttf/lib/libSDL2_ttf.dll.a")

# 拷贝运行时 DLL 到构建目录
add_custom_command(TARGET vplayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FFMPEG_ROOT}/bin/avcodec-63.dll"
    "${FFMPEG_ROOT}/bin/avformat-63.dll"
    "${FFMPEG_ROOT}/bin/avutil-61.dll"
    "${FFMPEG_ROOT}/bin/swresample-7.dll"
    "${FFMPEG_ROOT}/bin/swscale-10.dll"
    "${DEV_DIR}/sdl2/x86_64-w64-mingw32/bin/SDL2.dll"
    "${DEV_DIR}/sdl2_image/x86_64-w64-mingw32/bin/SDL2_image.dll"
    "${DEV_DIR}/sdl2_ttf/bin/SDL2_ttf.dll"
    $<TARGET_FILE_DIR:vplayer>
  COMMENT "拷贝运行时 DLL")

# 拷贝图标资源到构建目录
add_custom_command(TARGET vplayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:vplayer>/assets"
  COMMENT "拷贝 assets 资源")
