add_executable(semilive_relay_session_test
    relay_session_test.cpp
)
target_link_libraries(semilive_relay_session_test PRIVATE
    SemiLive::RelayApplication
    SemiLive::RelayInfraNetwork
)
semilive_enable_warnings(semilive_relay_session_test)

add_test(NAME semilive_relay_session COMMAND semilive_relay_session_test)
