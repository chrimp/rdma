add_executable(send_recv send_recv.cpp)
add_executable(send_recv_perf send_recv_perf.cpp)

if (WIN32)
    target_link_libraries(send_recv PRIVATE NetworkDirect NDSession ws2_32)
    target_link_libraries(send_recv_perf PRIVATE NetworkDirect NDSession ws2_32)
endif()