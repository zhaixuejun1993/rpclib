cmake_minimum_required(VERSION 3.5.1)
project(rpc VERSION 2.3.0)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(policies)
include(msvc_support)
include(check_warning_flag)

#
# Options
#
option(RPCLIB_ENABLE_LOGGING 
  "ALlow logging in the library for debug purposes."
  OFF)
option(ENABLE_ITT "Parameter to enable itt tracer" ON)

include_directories(${CMAKE_CURRENT_BINARY_DIR}/configs)

set(X86_ENV ON)

#
# Other configuration values
#
# to do remove (replace by IPC)
set(RPCLIB_DEFAULT_PORT 8080 
  CACHE STRING "Default port used for running tests and examples")
set(RPCLIB_DEFAULT_BUFFER_SIZE "1024 << 10" 
  CACHE STRING "Default buffer size")
set(RPCLIB_CXX_STANDARD 11 CACHE STRING 
  "C++ version used to build rpclib (Currently: Only 11 and 14 supported)")

if(NOT ${RPCLIB_CXX_STANDARD} EQUAL 14 AND 
   NOT ${RPCLIB_CXX_STANDARD} EQUAL 11)
  message(fatal_error "Unsupported C++ standard: ${RPCLIB_CXX_STANDARD}")
endif()

set(CMAKE_CXX_STANDARD ${RPCLIB_CXX_STANDARD})

#
# Compile & install the library
#
if (WIN32)
  set(RPCLIB_OS_DEF "RPCLIB_WIN32")
elseif (LINUX)
  set(RPCLIB_OS_DEF "RPCLIB_LINUX")
elseif (APPLE)
  set(RPCLIB_OS_DEF "RPCLIB_MAC")
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/include/rpc/config.h.in"
  "${PROJECT_SOURCE_DIR}/include/rpc/config.h")

file(GLOB_RECURSE RPCLIB_HEADERS
  include/rpc/*.h
  include/msgpack/*.hpp)

if (MSVC)
  SET(IPC_SRC lib/rpc/platform/windows/IPC.cpp)
else()
  SET(IPC_SRC lib/rpc/platform/linux/IPC.cpp)
endif()

add_subdirectory(thirdparty/ittnotify)

add_library(${PROJECT_NAME}
  lib/rpc/dispatcher.cc
  lib/rpc/server.cc
  lib/rpc/client.cc
  lib/rpc/this_handler.cc
  lib/rpc/this_session.cc
  lib/rpc/this_server.cc
  lib/rpc/rpc_error.cc
  lib/rpc/ITTProfiler.cpp
  lib/rpc/detail/server_session.cc
  lib/rpc/detail/response.cc
  lib/rpc/detail/client_error.cc
  lib/rpc/nonstd/optional.cc
  lib/rpc/posix.cc
  lib/rpc/format.cc
  ${IPC_SRC}
  ${RPCLIB_HEADERS})

# Perform steps and checks required for MSVC support
rpclib_msvc_support()

target_compile_definitions(${PROJECT_NAME} 
  PRIVATE
    "${RPCLIB_COMPILE_DEFINITIONS}"
    "${RPCLIB_ARCH_DEF}"
    "RPCLIB_FMT=clmdep_fmt"
  PUBLIC
    "${RPCLIB_OS_DEF}"
    "RPCLIB_MSGPACK=clmdep_msgpack"
  )

if(RPCLIB_ENABLE_LOGGING)
  target_compile_definitions(${PROJECT_NAME} PRIVATE "RPCLIB_ENABLE_LOGGING")
endif()

#to do: remove
if(RPCLIB_COMPILE_DEFINITIONS)
  set_target_properties(${PROJECT_NAME}
    PROPERTIES
    COMPILE_DEFINITIONS "${RPCLIB_COMPILE_DEFINITIONS}")
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC libittprofiler)
if (WIN32)
  target_link_libraries(${PROJECT_NAME} wsock32 ws2_32)
endif()
target_include_directories(
  ${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  )

