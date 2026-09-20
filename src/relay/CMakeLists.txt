add_subdirectory(domain)
add_subdirectory(infrastructure/network)
add_subdirectory(application)
add_subdirectory(reporting)

semilive_add_app(semilive_relay main.cpp)
target_link_libraries(semilive_relay PRIVATE
    SemiLive::CommonLog
    SemiLive::RelayApplication
    SemiLive::RelayReporting
)
