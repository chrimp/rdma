add_executable(send_recv send_recv.cpp)

if (WIN32)
    target_link_libraries(send_recv PRIVATE NetworkDirect MyND2Class ws2_32)
endif()