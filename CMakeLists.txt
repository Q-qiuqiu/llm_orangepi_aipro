cmake_minimum_required(VERSION 3.2)

project(qwen_server CXX)
cmake_policy(SET CMP0128 OLD)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")

add_subdirectory(src)
