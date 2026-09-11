add_library(semilive_publisher_composition STATIC
    src/publisher_composition.cpp
)

add_library(SemiLive::PublisherComposition ALIAS
    semilive_publisher_composition
)

target_include_directories(semilive_publisher_composition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_composition PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_composition
    PUBLIC
        SemiLive::PublisherApplication
    PRIVATE
        SemiLive::PublisherDomain
        SemiLive::PublisherInfraCapture
        SemiLive::PublisherInfraFfmpeg
        SemiLive::PublisherInfraNotifier
        SemiLive::PublisherInfraOutput
)
semilive_enable_warnings(semilive_publisher_composition)
