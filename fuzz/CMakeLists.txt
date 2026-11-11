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

add_executable(minidb_wal_record_fuzz
    wal_record_fuzz.cpp
)

target_link_libraries(minidb_wal_record_fuzz PRIVATE
    minidb_storage
    minidb_base
)

target_compile_options(minidb_wal_record_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)

target_link_options(minidb_wal_record_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)

add_executable(minidb_checkpoint_manifest_fuzz
    checkpoint_manifest_fuzz.cpp
)

target_link_libraries(minidb_checkpoint_manifest_fuzz PRIVATE
    minidb_storage
    minidb_base
)

target_compile_options(minidb_checkpoint_manifest_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)

target_link_options(minidb_checkpoint_manifest_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)

add_executable(minidb_agent_record_codec_fuzz
    agent_record_codec_fuzz.cpp
)

target_link_libraries(minidb_agent_record_codec_fuzz PRIVATE
    minidb_storage
    minidb_base
)

target_compile_options(minidb_agent_record_codec_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)

target_link_options(minidb_agent_record_codec_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
)
