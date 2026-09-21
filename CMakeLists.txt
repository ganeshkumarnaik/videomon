cmake_minimum_required(VERSION 3.16)
project(videomon)

#Set C++ standards and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# output directory build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find packages
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# Add executable
add_executable(videomon ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

#Include dirs
target_include_directories(videomon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

#Link libraries
target_link_libraries(videomon 
    PRIVATE
        Threads::Threads
        CURL::libcurl)
