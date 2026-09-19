add_library(semilive_receiver_reporting STATIC
    src/receiver_session_report.cpp
)

add_library(SemiLive::ReceiverReporting ALIAS
    semilive_receiver_reporting
)

target_include_directories(semilive_receiver_reporting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_receiver_reporting PUBLIC cxx_std_23)
target_link_libraries(semilive_receiver_reporting PUBLIC
    SemiLive::ReceiverComposition
)
semilive_enable_warnings(semilive_receiver_reporting)
