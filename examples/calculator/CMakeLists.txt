cmake_minimum_required(VERSION 3.0.0)
project(calculator)

find_package(Threads REQUIRED)

include_directories(${PROJECT_SOURCE_DIR}/include)

add_executable(calculator_server calculator_server.cc)
target_link_libraries(calculator_server rpc ${CMAKE_THREAD_LIBS_INIT})


add_executable(calculator_client calculator_client.cc)
target_link_libraries(calculator_client rpc ${CMAKE_THREAD_LIBS_INIT})

