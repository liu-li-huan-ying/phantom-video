cmake_minimum_required(VERSION 3.16)
project(phantom_video CXX)

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

# libmpv
set(MPV_ROOT "${DEV_DIR}/mpv")
set(MPV_INCLUDE_DIRS "${MPV_ROOT}/include")
set(MPV_LIB "${MPV_ROOT}/libmpv.dll.a")

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Sonic 库
enable_language(C)
add_library(sonic STATIC "${DEV_DIR}/sonic/sonic.c")
target_include_directories(sonic PUBLIC "${DEV_DIR}/sonic")

add_executable(phantom_video ${PLAYER_SOURCES})

target_include_directories(phantom_video PRIVATE
  ${SDL2_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
  ${MPV_INCLUDE_DIRS}
  "${DEV_DIR}/sonic"
  ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(phantom_video PRIVATE SDL_MAIN_HANDLED)

# UTF-8 全链路: 源文件 → 编译器 → 可执行文件 → GDI 渲染
target_compile_options(phantom_video PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)

# 过滤 SDL2main 相关标志
set(SDL2_LINK "")
foreach(f ${SDL2_LDFLAGS})
  if(NOT f MATCHES "^-lSDL2main$|^-lmingw32$|^-mwindows$")
    list(APPEND SDL2_LINK ${f})
  endif()
endforeach()
foreach(f ${SDL2_CFLAGS_OTHER})
  if(NOT f MATCHES "^-Dmain=")
    target_compile_options(phantom_video PRIVATE ${f})
  endif()
endforeach()

target_link_libraries(phantom_video PRIVATE ${SDL2_LINK} ${FFMPEG_LIBS} sonic ${MPV_LIB} shell32 dwmapi msimg32
  "${DEV_DIR}/sdl2_image/x86_64-w64-mingw32/lib/libSDL2_image.dll.a"
  "${DEV_DIR}/sdl2_ttf/lib/libSDL2_ttf.dll.a")

# 拷贝运行时 DLL 到构建目录
add_custom_command(TARGET phantom_video POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FFMPEG_ROOT}/bin/avcodec-63.dll"
    "${FFMPEG_ROOT}/bin/avformat-63.dll"
    "${FFMPEG_ROOT}/bin/avutil-61.dll"
    "${FFMPEG_ROOT}/bin/swresample-7.dll"
    "${FFMPEG_ROOT}/bin/swscale-10.dll"
    "${DEV_DIR}/sdl2/x86_64-w64-mingw32/bin/SDL2.dll"
    "${DEV_DIR}/sdl2_image/x86_64-w64-mingw32/bin/SDL2_image.dll"
    "${DEV_DIR}/sdl2_ttf/bin/SDL2_ttf.dll"
    "${MPV_ROOT}/libmpv-2.dll"
    $<TARGET_FILE_DIR:phantom_video>
  COMMENT "拷贝运行时 DLL")

# 拷贝图标资源到构建目录
add_custom_command(TARGET phantom_video POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:phantom_video>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/ico"
    "$<TARGET_FILE_DIR:phantom_video>/ico"
  COMMENT "拷贝 assets 资源")
