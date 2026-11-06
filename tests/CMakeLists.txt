function(minidb_discover_tests target)
    gtest_discover_tests(${target}
        DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT 120 ${ARGN}
    )
endfunction()

add_executable(base_test
    FileDescriptorTest.cpp
    BufferTest.cpp
    ClockTest.cpp
    LogTest.cpp
    ErrorTest.cpp
    HttpParserP0Test.cpp
    ContentLengthParserTest.cpp
)

target_link_libraries(base_test
    minidb_protocol
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(base_test)

add_executable(storage_test
    KvStoreTtlTest.cpp
    KvStoreTtlStressTest.cpp
    RdbManagerFaultTest.cpp
    RdbFormatTest.cpp
    RdbManagerTest.cpp
    RdbManagerP0Test.cpp
)

target_link_libraries(storage_test
    minidb_storage
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(storage_test)

add_executable(integration_test
    IntegrationTest.cpp
)

target_link_libraries(integration_test
    minidb_protocol
    minidb_storage
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(integration_test RESOURCE_LOCK minidb_network)

# HTTP Keep-Alive and socket/session integration tests.
add_executable(http_keepalive_test
    HttpKeepAliveTest.cpp
)

target_link_libraries(http_keepalive_test
    minidb_protocol
    minidb_storage
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(http_keepalive_test RESOURCE_LOCK minidb_network)

add_executable(http_session_integration_test
    HttpSessionIntegrationTest.cpp
)

target_link_libraries(http_session_integration_test
    minidb_protocol
    minidb_storage
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(http_session_integration_test RESOURCE_LOCK minidb_network)

add_executable(kv_protocol_test
    KvProtocolTest.cpp
)

target_link_libraries(kv_protocol_test
    minidb_protocol
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(kv_protocol_test)

add_executable(kv_session_integration_test
    KvSessionIntegrationTest.cpp
)

target_link_libraries(kv_session_integration_test
    minidb_protocol
    minidb_storage
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(kv_session_integration_test RESOURCE_LOCK minidb_network)

add_executable(event_loop_timer_test
    EventLoopTimerTest.cpp
)

target_link_libraries(event_loop_timer_test
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(event_loop_timer_test)

add_executable(net_unit_test
    AcceptorTest.cpp
    ChannelTest.cpp
    PollerTest.cpp
    TcpConnectionTest.cpp
)

target_link_libraries(net_unit_test
    minidb_net
    minidb_base
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(net_unit_test RESOURCE_LOCK minidb_network)

add_executable(server_options_test
    ServerOptionsTest.cpp
)

target_link_libraries(server_options_test
    GTest::gtest
    GTest::gtest_main
)

minidb_discover_tests(server_options_test)


find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(MINIDB_PROCESS_CASES
    CliContract
    HttpPersistence
    KvPersistence
    CorruptRdbStartup
    SaveFailureExit
)
foreach(process_case IN LISTS MINIDB_PROCESS_CASES)
    add_test(
        NAME ProcessIntegrationTest.${process_case}
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/process_integration_test.py"
                --http "$<TARGET_FILE:minidb_http_server>"
                --kv "$<TARGET_FILE:minidb_kv_server>"
                --case "${process_case}"
    )
    set_tests_properties(ProcessIntegrationTest.${process_case} PROPERTIES
        TIMEOUT 60
        RESOURCE_LOCK minidb_network
    )
endforeach()
