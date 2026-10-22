cmake_minimum_required(VERSION 3.16)
project(pg_systemone C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find libcurl
find_package(CURL REQUIRED)

# Find PostgreSQL
# Support standard FindPostgreSQL or manual PG_ROOT / PostgreSQL_ROOT override
if(DEFINED ENV{PG_ROOT} AND NOT DEFINED PostgreSQL_ROOT)
    set(PostgreSQL_ROOT $ENV{PG_ROOT})
endif()

find_package(PostgreSQL)

if(NOT PostgreSQL_FOUND)
    # Manual fallback for Windows installations (e.g. C:/Program Files/PostgreSQL/17)
    set(PG_DEFAULT_SEARCH_PATHS
        "C:/Program Files/PostgreSQL/17"
        "C:/Program Files/PostgreSQL/16"
        "C:/Program Files/PostgreSQL/15"
        "C:/Program Files/PostgreSQL/14"
        "$ENV{ProgramFiles}/PostgreSQL/17"
        "$ENV{ProgramFiles}/PostgreSQL/16"
        "$ENV{ProgramFiles}/PostgreSQL/15"
        "$ENV{ProgramFiles}/PostgreSQL/14"
    )

    find_path(PG_INCLUDE_SERVER_DIR
        NAMES "postgres.h"
        PATHS ${PG_DEFAULT_SEARCH_PATHS}
        PATH_SUFFIXES "include/server"
    )

    find_library(PG_LIB
        NAMES postgres
        PATHS ${PG_DEFAULT_SEARCH_PATHS}
        PATH_SUFFIXES "lib"
    )

    if(PG_INCLUDE_SERVER_DIR AND PG_LIB)
        get_filename_component(PG_INCLUDE_BASE_DIR "${PG_INCLUDE_SERVER_DIR}/.." ABSOLUTE)
        set(PostgreSQL_INCLUDE_DIRS
            "${PG_INCLUDE_BASE_DIR}"
            "${PG_INCLUDE_SERVER_DIR}"
            "${PG_INCLUDE_SERVER_DIR}/port/win32"
            "${PG_INCLUDE_SERVER_DIR}/port/win32_msvc"
        )
        set(PostgreSQL_LIBRARIES "${PG_LIB}")
        set(PostgreSQL_FOUND TRUE)
        message(STATUS "PostgreSQL manually detected at ${PG_INCLUDE_SERVER_DIR}")
    else()
        message(FATAL_ERROR "PostgreSQL server development headers/libraries not found. Set -DPostgreSQL_ROOT=\"C:/path/to/PostgreSQL/17\"")
    endif()
else()
    # Ensure server and win32 port headers are included
    list(APPEND PostgreSQL_INCLUDE_DIRS
        "${PostgreSQL_INCLUDE_DIRS}/server"
        "${PostgreSQL_INCLUDE_DIRS}/server/port/win32"
        "${PostgreSQL_INCLUDE_DIRS}/server/port/win32_msvc"
    )
endif()

add_library(pg_systemone SHARED
    src/pg_systemone.c
    src/cJSON.c
)

target_include_directories(pg_systemone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PostgreSQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_compile_definitions(pg_systemone PRIVATE
    WIN32
    _WINDOWS
    PG_MODULE_MAGIC
)

if(MSVC)
    target_compile_options(pg_systemone PRIVATE /W3 /wd4996)
endif()

target_link_libraries(pg_systemone PRIVATE
    ${PostgreSQL_LIBRARIES}
    CURL::libcurl
)

set_target_properties(pg_systemone PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pg_systemone"
    SUFFIX ".dll"
)

# Installation targets for Windows
install(TARGETS pg_systemone
    RUNTIME DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES
    pg_systemone.control
    pg_systemone--1.0.sql
    DESTINATION share/extension
)
