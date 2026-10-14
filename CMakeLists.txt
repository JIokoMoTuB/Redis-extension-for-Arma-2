cmake_minimum_required(VERSION 3.15)
project(RedisExt CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Arma 2 and DayZ require a 32-bit (x86) dynamic link library
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "RedisExt must be compiled as 32-bit (x86) for Arma 2 / DayZ server compatibility!")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# Locate hiredis headers and library
find_path(HIREDIS_INCLUDE_DIR
    NAMES hiredis/hiredis.h
    PATHS
        "$ENV{VCPKG_ROOT}/installed/x86-windows/include"
        "$ENV{USERPROFILE}/vcpkg/installed/x86-windows/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

find_library(HIREDIS_LIBRARY
    NAMES hiredis
    PATHS
        "$ENV{VCPKG_ROOT}/installed/x86-windows/lib"
        "$ENV{USERPROFILE}/vcpkg/installed/x86-windows/lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis not found! Please run 'vcpkg install hiredis:x86-windows' or set CMAKE_PREFIX_PATH.")
endif()

message(STATUS "hiredis include: ${HIREDIS_INCLUDE_DIR}")
message(STATUS "hiredis library: ${HIREDIS_LIBRARY}")

# Define shared library target
add_library(RedisExt SHARED
    src/dllmain.cpp
)

target_include_directories(RedisExt PRIVATE
    ${HIREDIS_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(RedisExt PRIVATE
    ${HIREDIS_LIBRARY}
    wininet
)

if(MSVC)
    target_compile_options(RedisExt PRIVATE /W3 /wd4200 /wd4244 /EHsc /O2)
    set_target_properties(RedisExt PROPERTIES
        OUTPUT_NAME "RedisExt"
        PREFIX ""
    )
endif()
