cmake_minimum_required(VERSION 3.16)
project(phantom_video CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(MPV REQUIRED mpv)

file(GLOB_RECURSE PLAYER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

enable_language(C)
add_library(sonic STATIC "${CMAKE_SOURCE_DIR}/dev/sonic/sonic.c")
target_include_directories(sonic PUBLIC "${CMAKE_SOURCE_DIR}/dev/sonic")

add_executable(phantom_video WIN32 ${PLAYER_SOURCES} app.rc)

target_include_directories(phantom_video PRIVATE
  ${SDL2_INCLUDE_DIRS}
  ${AVFORMAT_INCLUDE_DIRS}
  ${AVCODEC_INCLUDE_DIRS}
  ${AVUTIL_INCLUDE_DIRS}
  ${SWRESAMPLE_INCLUDE_DIRS}
  ${SWSCALE_INCLUDE_DIRS}
  ${MPV_INCLUDE_DIRS}
  "${CMAKE_SOURCE_DIR}/dev/sonic"
  ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(phantom_video PRIVATE SDL_MAIN_HANDLED)

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

target_link_libraries(phantom_video PRIVATE
  ${SDL2_LINK}
  ${AVFORMAT_LIBRARIES}
  ${AVCODEC_LIBRARIES}
  ${AVUTIL_LIBRARIES}
  ${SWRESAMPLE_LIBRARIES}
  ${SWSCALE_LIBRARIES}
  ${MPV_LIBRARIES}
  sonic shell32 dwmapi msimg32 dxgi)

target_link_directories(phantom_video PRIVATE
  ${AVFORMAT_LIBRARY_DIRS}
  ${AVCODEC_LIBRARY_DIRS}
  ${AVUTIL_LIBRARY_DIRS}
  ${SWRESAMPLE_LIBRARY_DIRS}
  ${SWSCALE_LIBRARY_DIRS}
  ${MPV_LIBRARY_DIRS})

# 拷贝运行时 DLL 到构建目录（通过 pkg-config 定位）
execute_process(
  COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=prefix libavformat
  OUTPUT_VARIABLE _ffprefix OUTPUT_STRIP_TRAILING_WHITESPACE)

foreach(codec avcodec avformat avutil swresample swscale)
  file(GLOB _ffdlls "${_ffprefix}/bin/${codec}-*.dll")
  foreach(dll ${_ffdlls})
    add_custom_command(TARGET phantom_video POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:phantom_video>)
  endforeach()
endforeach()

execute_process(
  COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=prefix sdl2
  OUTPUT_VARIABLE _sdl2prefix OUTPUT_STRIP_TRAILING_WHITESPACE)
file(GLOB _sdl2dll "${_sdl2prefix}/bin/SDL2.dll")
if(_sdl2dll)
  add_custom_command(TARGET phantom_video POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sdl2dll} $<TARGET_FILE_DIR:phantom_video>)
endif()

execute_process(
  COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=prefix mpv
  OUTPUT_VARIABLE _mpvprefix OUTPUT_STRIP_TRAILING_WHITESPACE)
file(GLOB _mpvdll "${_mpvprefix}/bin/libmpv-*.dll")
if(_mpvdll)
  add_custom_command(TARGET phantom_video POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mpvdll} $<TARGET_FILE_DIR:phantom_video>)
endif()

# 拷贝图标资源到构建目录
add_custom_command(TARGET phantom_video POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/ico"
    "$<TARGET_FILE_DIR:phantom_video>/ico"
  COMMENT "拷贝 ico 资源")
