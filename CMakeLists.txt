cmake_minimum_required(VERSION 3.20)

project(LogBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 获取跨平台的线程编译和链接选项。
find_package(Threads REQUIRED)

add_executable(logbridge
        src/FileTailReader.cpp
        src/Protocol.cpp
        src/TcpClient.cpp
        src/main.cpp
)

add_executable(logbridge_server
        src/Protocol.cpp
        src/TcpServer.cpp
        src/ServerMain.cpp
)

# 添加项目头文件目录。
target_include_directories(logbridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(logbridge_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(logbridge PRIVATE
        Threads::Threads
)

target_link_libraries(logbridge_server PRIVATE
        Threads::Threads
)

target_compile_options(logbridge PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(logbridge_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

include(CTest)

if (BUILD_TESTING)
    add_executable(file_tail_reader_tests
            tests/FileTailReaderTest.cpp
            src/FileTailReader.cpp
    )

    target_include_directories(file_tail_reader_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_options(file_tail_reader_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(
            NAME file_tail_reader
            COMMAND file_tail_reader_tests
    )

    add_executable(protocol_tests
            tests/ProtocolTest.cpp
            src/Protocol.cpp
    )

    target_include_directories(protocol_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_options(protocol_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(
            NAME protocol
            COMMAND protocol_tests
    )

    add_executable(tcp_transport_tests
            tests/TcpTransportTest.cpp
            src/Protocol.cpp
            src/TcpClient.cpp
            src/TcpServer.cpp
    )

    target_include_directories(tcp_transport_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(tcp_transport_tests PRIVATE
            Threads::Threads
    )

    target_compile_options(tcp_transport_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(
            NAME tcp_transport
            COMMAND tcp_transport_tests
    )
endif ()
