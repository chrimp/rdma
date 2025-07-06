add_executable(read_write read_write.cpp)

if (WIN32)
    target_link_libraries(read_write PRIVATE NetworkDirect NDSession ws2_32)
endif()