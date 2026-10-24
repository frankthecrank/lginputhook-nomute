cmake_minimum_required(VERSION 3.30)
project(inputhookpp)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP NEW)
FetchContent_MakeAvailable(json)

find_package(ezinject REQUIRED)

add_ezinject_library(inputhookpp
    USE_LH
    SOURCES inputhook.cpp keybindings.cpp
)

set_target_properties(inputhookpp PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_options(inputhookpp PRIVATE -ffunction-sections -fdata-sections)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FRIDA_GUM_STATIC
        REQUIRED
        IMPORTED_TARGET frida-gum-1.0 GLOBAL
)

macro(static_library_name var name)
    set(${var} ${CMAKE_STATIC_LIBRARY_PREFIX}${name}${CMAKE_STATIC_LIBRARY_SUFFIX})
endmacro()

function(find_static_library var name)
    static_library_name(_lib_name "${name}")
    unset(_lib_path CACHE)
    find_library(_lib_path NAMES ${_lib_name})
    set(${var} ${_lib_path} PARENT_SCOPE)
endfunction()

get_property(_frida_libs TARGET PkgConfig::FRIDA_GUM_STATIC PROPERTY INTERFACE_LINK_LIBRARIES)

set(_frida_libraries "")
set(_frida_libs "${_frida_libs};libffi;libpcre2-8;libdwarf")
message( STATUS "frida libs: ${_frida_libs}")

# construct new list
foreach(lib ${_frida_libs})
    get_filename_component(lib_name "${lib}" NAME)
    set(_old_lib_filename "${lib_name}")

    string(REGEX REPLACE "^${CMAKE_SHARED_LIBRARY_PREFIX}" "" lib_name "${lib_name}")
    string(REGEX REPLACE "^${CMAKE_STATIC_LIBRARY_PREFIX}" "" lib_name "${lib_name}")
    string(REGEX REPLACE "${CMAKE_SHARED_LIBRARY_SUFFIX}$" "" lib_name "${lib_name}")
    string(REGEX REPLACE "${CMAKE_STATIC_LIBRARY_SUFFIX}$" "" lib_name "${lib_name}")

    set(var_name "${lib_name}_LIBRARY")
    if("${lib_name}" IN_LIST _static_excluded)
        find_library(${var_name} ${lib_name})
    else()
        find_static_library(${var_name} ${lib_name})
    endif()
    set(lib "${${var_name}}")

    get_filename_component(_new_lib_filename "${lib}" NAME)
    if(NOT "${_old_lib_filename}" STREQUAL "${_new_lib_filename}")
        message(STATUS "frida ${lib_name} : ${_old_lib_filename} => ${_new_lib_filename}")
    endif()
    list(APPEND _frida_libraries "${lib}")
endforeach()

# set new list
set_target_properties(PkgConfig::FRIDA_GUM_STATIC PROPERTIES INTERFACE_LINK_LIBRARIES "${_frida_libraries}")

target_link_libraries(inputhookpp PkgConfig::FRIDA_GUM_STATIC)
target_include_directories(inputhookpp PRIVATE ${FRIDA_GUM_INCLUDE_DIRS})
# Bind bundled runtime and bootstrap symbols to this library.
target_link_options(inputhookpp PRIVATE
    "-static-libstdc++"
    "-Wl,-Bsymbolic"
    "-Wl,--exclude-libs,ALL"
    "-Wl,--gc-sections"
    "$<$<CONFIG:Release>:-Wl,-s>"
)
target_link_libraries(inputhookpp nlohmann_json::nlohmann_json)
