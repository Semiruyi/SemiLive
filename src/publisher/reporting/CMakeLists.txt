add_library(semilive_publisher_reporting STATIC
    src/publisher_session_report.cpp
)

add_library(SemiLive::PublisherReporting ALIAS
    semilive_publisher_reporting
)

target_include_directories(semilive_publisher_reporting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_reporting PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_reporting PUBLIC
    SemiLive::PublisherComposition
)
semilive_enable_warnings(semilive_publisher_reporting)
