add_library(semilive_relay_domain STATIC
    src/random_loss_policy.cpp
)

add_library(SemiLive::RelayDomain ALIAS semilive_relay_domain)

target_include_directories(semilive_relay_domain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(semilive_relay_domain PUBLIC cxx_std_23)
semilive_enable_warnings(semilive_relay_domain)
