cmake_minimum_required(VERSION 3.23)

project(SemiLive VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SEMILIVE_BUILD_TESTS "Build SemiLive tests" ON)

function(semilive_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
    endif()
endfunction()

function(semilive_add_app target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_23)
    semilive_enable_warnings(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

include(CTest)

add_subdirectory(src)

if(BUILD_TESTING AND SEMILIVE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
