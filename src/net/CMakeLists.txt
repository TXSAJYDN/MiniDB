add_library(minidb_net
    InetAddress.cpp
    Socket.cpp
    Channel.cpp
    Poller.cpp
    EventLoop.cpp
    Acceptor.cpp
    TcpConnection.cpp
    TcpServer.cpp
)

target_include_directories(minidb_net PUBLIC )
target_link_libraries(minidb_net PUBLIC minidb_base)
