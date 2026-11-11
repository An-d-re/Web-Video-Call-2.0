cmake_minimum_required(VERSION 3.13)
project(webrtc_signaling_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 运行时通过相对路径读取 ./static 与 turn_config.json，
# 所以可执行文件放在项目根目录最省事。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------- jsoncpp ----------
# Ubuntu/Debian: apt install libjsoncpp-dev
find_package(jsoncpp CONFIG QUIET)
if(TARGET jsoncpp_lib)
  set(JSONCPP_TARGET jsoncpp_lib)
elseif(TARGET JsonCpp::JsonCpp)
  set(JSONCPP_TARGET JsonCpp::JsonCpp)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(JSONCPP REQUIRED jsoncpp)
  add_library(jsoncpp_external INTERFACE)
  target_include_directories(jsoncpp_external INTERFACE ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(jsoncpp_external INTERFACE ${JSONCPP_LIBRARY_DIRS})
  target_link_libraries(jsoncpp_external INTERFACE ${JSONCPP_LIBRARIES})
  set(JSONCPP_TARGET jsoncpp_external)
  message(STATUS "jsoncpp: 使用 pkg-config (${JSONCPP_VERSION})")
endif()

# ---------- muduo ----------
# muduo 不在发行版软件源里，必须先自行编译安装，见 README「依赖」一节。
# 装在非标准路径时用 -DMUDUO_ROOT=/opt/muduo 指定。
find_path(MUDUO_INCLUDE_DIR
  NAMES muduo/net/TcpServer.h
  HINTS ${MUDUO_ROOT} ENV MUDUO_ROOT
  PATHS /usr/local /usr /opt/muduo
  PATH_SUFFIXES include)

find_library(MUDUO_NET_LIB
  NAMES muduo_net
  HINTS ${MUDUO_ROOT} ENV MUDUO_ROOT
  PATHS /usr/local /usr /opt/muduo
  PATH_SUFFIXES lib lib64)

find_library(MUDUO_BASE_LIB
  NAMES muduo_base
  HINTS ${MUDUO_ROOT} ENV MUDUO_ROOT
  PATHS /usr/local /usr /opt/muduo
  PATH_SUFFIXES lib lib64)

if(NOT MUDUO_INCLUDE_DIR OR NOT MUDUO_NET_LIB OR NOT MUDUO_BASE_LIB)
  message(FATAL_ERROR
    "找不到 muduo。请先编译安装 muduo（见 README），"
    "或用 -DMUDUO_ROOT=<muduo 安装前缀> 指定路径。\n"
    "  MUDUO_INCLUDE_DIR = ${MUDUO_INCLUDE_DIR}\n"
    "  MUDUO_NET_LIB     = ${MUDUO_NET_LIB}\n"
    "  MUDUO_BASE_LIB    = ${MUDUO_BASE_LIB}")
endif()
message(STATUS "muduo 头文件: ${MUDUO_INCLUDE_DIR}")

# muduo_net 依赖 zlib（ZlibStream）；静态链接时需要显式带上
find_package(ZLIB QUIET)

add_executable(server server.cpp jwt_verify.cpp)

target_include_directories(server PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${MUDUO_INCLUDE_DIR}
  ${JSONCPP_INCLUDE_DIRS})

# server.cpp 用到 OpenSSL 1.x 风格的 SHA1()/BIO 接口，在 OpenSSL 3.x 上会告警
target_compile_definitions(server PRIVATE _GNU_SOURCE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(server PRIVATE -Wall -Wno-deprecated-declarations)
endif()

target_link_libraries(server PRIVATE
  ${MUDUO_NET_LIB}
  ${MUDUO_BASE_LIB}
  ${JSONCPP_TARGET}
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads)

if(ZLIB_FOUND)
  target_link_libraries(server PRIVATE ZLIB::ZLIB)
endif()

install(TARGETS server RUNTIME DESTINATION bin)

# ---------- JWT 交叉验证自检 ----------
# 不依赖 muduo，只用 OpenSSL + jsoncpp，所以可以单独构建：
#   cmake --build build --target jwt_selftest
#   ./jwt_selftest tests/jwt_vectors.json
add_executable(jwt_selftest tests/jwt_selftest.cpp jwt_verify.cpp)
target_include_directories(jwt_selftest PRIVATE ${CMAKE_SOURCE_DIR} ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(jwt_selftest PRIVATE ${JSONCPP_TARGET} OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(jwt_selftest PRIVATE -Wall -Wno-deprecated-declarations)
endif()
