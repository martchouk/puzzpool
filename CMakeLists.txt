cmake_minimum_required(VERSION 3.20)

# Silence the warnings you saw with newer CMake
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(puzzpool_server
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PUZZPOOL_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(PUZZPOOL_USE_SYSTEM_CROW "Use installed Crow package instead of bundled third_party/crow" OFF)
option(PUZZPOOL_USE_SYSTEM_SQLITECPP "Use installed SQLiteCpp package instead of bundled third_party/SQLiteCpp" OFF)
option(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS "Suppress warnings from bundled/system third-party code" ON)

add_executable(puzzpool
    src/main.cpp
    src/env.cpp
    src/config.cpp
    src/hex_bigint.cpp
    src/hash_utils.cpp
    src/permutation.cpp
    src/db.cpp
    src/allocator.cpp
    src/service.cpp
)

target_include_directories(puzzpool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(puzzpool PRIVATE cxx_std_20)

# ------------------------------------------------------------
# Project warnings: apply ONLY to our target
# ------------------------------------------------------------
if(MSVC)
    target_compile_options(puzzpool PRIVATE
        /W4
        /permissive-
    )
    if(PUZZPOOL_WARNINGS_AS_ERRORS)
        target_compile_options(puzzpool PRIVATE /WX)
    endif()
else()
    target_compile_options(puzzpool PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )
    if(PUZZPOOL_WARNINGS_AS_ERRORS)
        target_compile_options(puzzpool PRIVATE -Werror)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(puzzpool PRIVATE Threads::Threads)

find_package(Boost REQUIRED)
target_link_libraries(puzzpool PRIVATE Boost::boost)

find_package(nlohmann_json REQUIRED)
target_link_libraries(puzzpool PRIVATE nlohmann_json::nlohmann_json)

# ------------------------------------------------------------
# Crow
# ------------------------------------------------------------
if(PUZZPOOL_USE_SYSTEM_CROW)
    find_package(Crow REQUIRED)
    target_link_libraries(puzzpool PRIVATE Crow::Crow)

    # If Crow is header-heavy, treat its includes as SYSTEM for this target.
    if(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS)
        get_target_property(_crow_includes Crow::Crow INTERFACE_INCLUDE_DIRECTORIES)
        if(_crow_includes)
            target_include_directories(puzzpool SYSTEM PRIVATE ${_crow_includes})
        endif()
    endif()
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include/crow.h")
        if(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS)
            target_include_directories(puzzpool SYSTEM PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include"
            )
        else()
            target_include_directories(puzzpool PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include"
            )
        endif()
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include/crow/app.h")
        if(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS)
            target_include_directories(puzzpool SYSTEM PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include"
            )
        else()
            target_include_directories(puzzpool PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crow/include"
            )
        endif()
    else()
        message(FATAL_ERROR
            "Crow not found.\n"
            "Expected bundled Crow under third_party/crow/include\n"
            "or configure with -DPUZZPOOL_USE_SYSTEM_CROW=ON"
        )
    endif()
endif()

# ------------------------------------------------------------
# SQLiteCpp
# ------------------------------------------------------------
if(PUZZPOOL_USE_SYSTEM_SQLITECPP)
    find_package(SQLite3 REQUIRED)
    find_package(SQLiteCpp REQUIRED)
    target_link_libraries(puzzpool PRIVATE SQLiteCpp SQLite::SQLite3)

    if(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS)
        # Treat imported include dirs as SYSTEM on our target
        get_target_property(_sqlitecpp_includes SQLiteCpp INTERFACE_INCLUDE_DIRECTORIES)
        if(_sqlitecpp_includes)
            target_include_directories(puzzpool SYSTEM PRIVATE ${_sqlitecpp_includes})
        endif()
    endif()
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SQLiteCpp/CMakeLists.txt")
        set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_RUN_DOXYGEN OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

        add_subdirectory(third_party/SQLiteCpp)

        if(TARGET SQLiteCpp)
            target_link_libraries(puzzpool PRIVATE SQLiteCpp)

            if(PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS)
                # Suppress warnings while building the vendored SQLiteCpp target itself
                if(MSVC)
                    target_compile_options(SQLiteCpp PRIVATE /W0)
                else()
                    target_compile_options(SQLiteCpp PRIVATE -w)
                endif()

                # And treat its public includes as system includes for our target
                get_target_property(_sqlitecpp_includes SQLiteCpp INTERFACE_INCLUDE_DIRECTORIES)
                if(_sqlitecpp_includes)
                    target_include_directories(puzzpool SYSTEM PRIVATE ${_sqlitecpp_includes})
                endif()

                # If bundled sqlite3 target exists, suppress warnings there too
                if(TARGET sqlite3)
                    if(MSVC)
                        target_compile_options(sqlite3 PRIVATE /W0)
                    else()
                        target_compile_options(sqlite3 PRIVATE -w)
                    endif()
                endif()
            endif()
        else()
            message(FATAL_ERROR "Bundled SQLiteCpp did not create target SQLiteCpp")
        endif()
    else()
        message(FATAL_ERROR
            "SQLiteCpp not found.\n"
            "Expected bundled SQLiteCpp at third_party/SQLiteCpp\n"
            "or configure with -DPUZZPOOL_USE_SYSTEM_SQLITECPP=ON"
        )
    endif()
endif()

if(APPLE)
    target_compile_definitions(puzzpool PRIVATE _DARWIN_C_SOURCE)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(puzzpool PRIVATE _GNU_SOURCE)
endif()

set_target_properties(puzzpool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS puzzpool
    RUNTIME DESTINATION bin
)

message(STATUS "========================================")
message(STATUS "Project:                         ${PROJECT_NAME}")
message(STATUS "Version:                         ${PROJECT_VERSION}")
message(STATUS "Build type:                      ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard:                    ${CMAKE_CXX_STANDARD}")
message(STATUS "Use system Crow:                 ${PUZZPOOL_USE_SYSTEM_CROW}")
message(STATUS "Use system SQLiteCpp:            ${PUZZPOOL_USE_SYSTEM_SQLITECPP}")
message(STATUS "Suppress third-party warnings:   ${PUZZPOOL_SUPPRESS_THIRD_PARTY_WARNINGS}")
message(STATUS "========================================")
