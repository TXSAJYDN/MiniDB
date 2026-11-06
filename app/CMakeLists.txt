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
