cmake_minimum_required(VERSION 3.22)
project(BCord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# so find_package() can see things you installed into /usr/local
list(APPEND CMAKE_PREFIX_PATH "/usr/local")


add_subdirectory(common)

if (WIN32)
    add_subdirectory(client)
endif()

if (UNIX AND NOT WIN32)
    add_subdirectory(server)
endif()
