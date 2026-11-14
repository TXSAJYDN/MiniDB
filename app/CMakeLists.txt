add_executable(minidb_http_server
    http_server_main.cpp
)

target_include_directories(minidb_http_server PRIVATE )
target_link_libraries(minidb_http_server PRIVATE
    minidb_protocol
    minidb_net
    minidb_storage
    minidb_base
)

# Test-only binary: installs the checkpoint RDB-publication hook behind a
# compile-time guard. The production minidb_http_server never contains it.
add_executable(minidb_http_test_server
    http_server_main.cpp
)
target_compile_definitions(minidb_http_test_server PRIVATE MINIDB_TEST_HOOKS=1)
target_link_libraries(minidb_http_test_server PRIVATE
    minidb_protocol
    minidb_net
    minidb_storage
    minidb_base
)

add_executable(minidb_kv_server
    kv_server_main.cpp
)

target_include_directories(minidb_kv_server PRIVATE )
target_link_libraries(minidb_kv_server PRIVATE
    minidb_protocol
    minidb_net
    minidb_storage
    minidb_base
)
