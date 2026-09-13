add_executable(semilive_receiver_datagram_source_contract_test
    datagram_source_backend_test.cpp
)
target_link_libraries(
    semilive_receiver_datagram_source_contract_test
    PRIVATE
        SemiLive::ReceiverContracts
)
semilive_enable_warnings(semilive_receiver_datagram_source_contract_test)

add_test(
    NAME semilive_receiver_datagram_source_contract
    COMMAND semilive_receiver_datagram_source_contract_test
)
