cmake_minimum_required(VERSION 3.5...3.29)

project(imdb CXX)

include_directories(include)

set(CMAKE_BUILD_TYPE "Release")
add_library(imdb STATIC imdb.cpp ${ALL_OBJECT_FILES})

disable_target_warnings(imdb)
