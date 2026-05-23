cmake_minimum_required(VERSION 3.20)

project(forcebindip_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(forcebindip_cpp
    src/launcher.cpp
)

add_library(bindip_hook SHARED
    src/hook_dll.cpp
)

target_compile_definitions(bindip_hook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(forcebindip_cpp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(bindip_hook PRIVATE Ws2_32 Psapi)
target_link_libraries(forcebindip_cpp PRIVATE Iphlpapi Ws2_32 Shell32)

if(MSVC)
    target_compile_options(forcebindip_cpp PRIVATE /W4 /permissive-)
    target_compile_options(bindip_hook PRIVATE /W4 /permissive-)
endif()
