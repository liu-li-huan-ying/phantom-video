cmake_minimum_required(VERSION 3.16)
project(vplayer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswresample)

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(vplayer ${PLAYER_SOURCES})

target_include_directories(vplayer PRIVATE
  ${SDL2_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(vplayer PRIVATE SDL_MAIN_HANDLED)

# 本项目用 SDL_MAIN_HANDLED + 普通 main，过滤 SDL2main 相关标志
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

target_link_libraries(vplayer PRIVATE ${SDL2_LINK} ${FFMPEG_LDFLAGS} shell32)

# 拷贝运行时 DLL 到构建目录（依赖根目录可通过 -DDEPS_ROOT= 覆盖）
set(DEPS_ROOT "F:/dev" CACHE PATH "FFmpeg/SDL2 依赖根目录")
add_custom_command(TARGET vplayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${DEPS_ROOT}/ffmpeg/bin/avcodec-63.dll"
    "${DEPS_ROOT}/ffmpeg/bin/avformat-63.dll"
    "${DEPS_ROOT}/ffmpeg/bin/avutil-61.dll"
    "${DEPS_ROOT}/ffmpeg/bin/swresample-7.dll"
    "${DEPS_ROOT}/ffmpeg/bin/swscale-10.dll"
    "${DEPS_ROOT}/sdl2/x86_64-w64-mingw32/bin/SDL2.dll"
    $<TARGET_FILE_DIR:vplayer>
  COMMENT "拷贝运行时 DLL")