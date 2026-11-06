add_library(minidb_protocol
    HttpRequest.cpp
    HttpResponse.cpp
    HttpParser.cpp
    HttpSession.cpp
    HttpServer.cpp
    KvProtocol.cpp
    KvSession.cpp
    KvServer.cpp
)

target_include_directories(minidb_protocol PUBLIC )
target_link_libraries(minidb_protocol PUBLIC minidb_net minidb_storage)
