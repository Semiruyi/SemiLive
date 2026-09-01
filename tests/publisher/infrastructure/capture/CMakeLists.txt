add_executable(semilive_publisher_synthetic_capture_test
    synthetic_desktop_capture_backend_test.cpp
)
target_link_libraries(semilive_publisher_synthetic_capture_test PRIVATE
    semilive_publisher_core
)
semilive_enable_warnings(semilive_publisher_synthetic_capture_test)

add_test(
    NAME semilive_publisher_synthetic_capture
    COMMAND semilive_publisher_synthetic_capture_test
)

if(WIN32)
    add_executable(semilive_publisher_dxgi_capture_test
        dxgi_desktop_capture_backend_test.cpp
    )
    target_link_libraries(semilive_publisher_dxgi_capture_test PRIVATE
        semilive_publisher_core
    )
    semilive_enable_warnings(semilive_publisher_dxgi_capture_test)

    add_test(
        NAME semilive_publisher_dxgi_capture
        COMMAND semilive_publisher_dxgi_capture_test
    )
endif()
