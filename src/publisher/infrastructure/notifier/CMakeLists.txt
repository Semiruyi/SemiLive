add_library(semilive_publisher_infra_notifier STATIC
    src/default_notifier.cpp
)

add_library(SemiLive::PublisherInfraNotifier ALIAS semilive_publisher_infra_notifier)

target_include_directories(semilive_publisher_infra_notifier PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_publisher_infra_notifier PUBLIC cxx_std_23)
target_link_libraries(semilive_publisher_infra_notifier
    PUBLIC
        SemiLive::PublisherContracts
    PRIVATE
        SemiLive::CommonLog
)
semilive_enable_warnings(semilive_publisher_infra_notifier)
