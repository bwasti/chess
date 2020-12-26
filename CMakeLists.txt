cmake_minimum_required(VERSION 3.19)
set(CMAKE_CXX_STANDARD 20)

project(brmbot)
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "-O3 -fno-exceptions")

add_executable(brmbot main.cc)

set (gflags_BUILD_STATIC_LIBS ON)
add_subdirectory(gflags)

file(GLOB stockfish_srcs ${CMAKE_SOURCE_DIR}/Stockfish/src/*.cpp)
file(GLOB syzygy_srcs ${CMAKE_SOURCE_DIR}/Stockfish/src/syzygy/*.cpp)
file(GLOB_RECURSE nnue_srcs ${CMAKE_SOURCE_DIR}/Stockfish/src/nnue/*.cpp)
set(CMAKE_CXX_FLAGS "-DNNUE_EMBEDDING_OFF")
add_library(stockfish STATIC 
 ${stockfish_srcs}
 ${syzygy_srcs}
 ${nnue_srcs}
)
include_directories(${CMAKE_SOURCE_DIR}/Stockfish/src/)
target_link_libraries(brmbot stockfish gflags)
