add_executable(semilive_relay_random_loss_policy_test
    random_loss_policy_test.cpp
)
target_link_libraries(semilive_relay_random_loss_policy_test PRIVATE
    SemiLive::RelayDomain
)
semilive_enable_warnings(semilive_relay_random_loss_policy_test)

add_test(
    NAME semilive_relay_random_loss_policy
    COMMAND semilive_relay_random_loss_policy_test
)
