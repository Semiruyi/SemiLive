function(semilive_target_add_logging target)
    find_package(spdlog CONFIG REQUIRED)

    target_sources(${target} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/log.cpp
    )
    target_link_libraries(${target} PUBLIC
        spdlog::spdlog
    )
endfunction()
