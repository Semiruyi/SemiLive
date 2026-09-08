add_library(semilive_publisher_infra_capture STATIC
    src/desktop_pointer_compositor.cpp
    src/synthetic_desktop_capture_backend.cpp
)

add_library(SemiLive::PublisherInfraCapture ALIAS semilive_publisher_infra_capture)

target_include_directories(semilive_publisher_infra_capture
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(semilive_publisher_infra_capture PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_infra_capture PUBLIC
    SemiLive::PublisherContracts
)

if(WIN32)
    target_sources(semilive_publisher_infra_capture PRIVATE
        src/dxgi_desktop_capture_backend.cpp
    )
    target_link_libraries(semilive_publisher_infra_capture PRIVATE
        d3d11
        dxgi
        ole32
    )
endif()

semilive_enable_warnings(semilive_publisher_infra_capture)
