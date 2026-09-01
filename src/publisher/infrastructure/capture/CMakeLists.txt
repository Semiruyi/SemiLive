target_sources(semilive_publisher_core PRIVATE
    synthetic_desktop_capture_backend.cpp
)

if(WIN32)
    target_sources(semilive_publisher_core PRIVATE
        dxgi_desktop_capture_backend.cpp
    )
    target_link_libraries(semilive_publisher_core PRIVATE
        d3d11
        dxgi
    )
endif()
