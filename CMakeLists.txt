cmake_minimum_required(VERSION 3.16)
project(vplayer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# FFmpeg 9.0.1（gyan.dev 稳定版 shared 构建）：无 pkg-config 文件，lib 为 COFF 导入库
set(FFMPEG_ROOT "F:/dev/ffmpeg-9.0.1-full_build-shared" CACHE PATH "FFmpeg 依赖目录")
set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
set(FFMPEG_LIBS
  "${FFMPEG_ROOT}/lib/avformat.lib"
  "${FFMPEG_ROOT}/lib/avcodec.lib"
  "${FFMPEG_ROOT}/lib/avutil.lib"
  "${FFMPEG_ROOT}/lib/swresample.lib"
  "${FFMPEG_ROOT}/lib/swscale.lib"
  "${FFMPEG_ROOT}/lib/avfilter.lib")

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(vplayer ${PLAYER_SOURCES})

target_include_directories(vplayer PRIVATE
  ${SDL2_INCLUDE_DIRS}
  "${DEPS_ROOT}/SDL2_image-2.8.2/x86_64-w64-mingw32/include/SDL2"
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

target_link_libraries(vplayer PRIVATE ${SDL2_LINK} ${FFMPEG_LIBS} shell32 dwmapi
  "${DEPS_ROOT}/SDL2_image-2.8.2/x86_64-w64-mingw32/lib/libSDL2_image.dll.a")

# 拷贝运行时 DLL 到构建目录（依赖根目录可通过 -DDEPS_ROOT= 覆盖）
set(DEPS_ROOT "F:/dev" CACHE PATH "FFmpeg/SDL2 依赖根目录")
add_custom_command(TARGET vplayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FFMPEG_ROOT}/bin/avcodec-63.dll"
    "${FFMPEG_ROOT}/bin/avformat-63.dll"
    "${FFMPEG_ROOT}/bin/avutil-61.dll"
    "${FFMPEG_ROOT}/bin/swresample-7.dll"
    "${FFMPEG_ROOT}/bin/swscale-10.dll"
    "${FFMPEG_ROOT}/bin/avfilter-12.dll"
    "${DEPS_ROOT}/sdl2/x86_64-w64-mingw32/bin/SDL2.dll"
    "${DEPS_ROOT}/SDL2_image-2.8.2/x86_64-w64-mingw32/bin/SDL2_image.dll"
    $<TARGET_FILE_DIR:vplayer>
  COMMENT "拷贝运行时 DLL")

# 拷贝图标资源到构建目录
add_custom_command(TARGET vplayer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:vplayer>/assets"
  COMMENT "拷贝 assets 资源")