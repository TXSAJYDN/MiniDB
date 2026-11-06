add_executable(minidb_http_parser_fuzz
    http_parser_fuzz.cpp
)

target_link_libraries(minidb_http_parser_fuzz PRIVATE
    minidb_protocol
    minidb_base
)

target_compile_options(minidb_http_parser_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)

target_link_options(minidb_http_parser_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)

add_executable(minidb_kv_protocol_fuzz
    kv_protocol_fuzz.cpp
)

target_link_libraries(minidb_kv_protocol_fuzz PRIVATE
    minidb_protocol
    minidb_base
)

target_compile_options(minidb_kv_protocol_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)

target_link_options(minidb_kv_protocol_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)
