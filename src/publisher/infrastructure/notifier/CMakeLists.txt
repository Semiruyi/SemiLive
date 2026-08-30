add_library(semilive_publisher_notifier STATIC
    default_notifier.cpp
)
target_include_directories(semilive_publisher_notifier PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(semilive_publisher_notifier PUBLIC cxx_std_23)
semilive_enable_warnings(semilive_publisher_notifier)
