file(GLOB NETWORKDIRECT_SOURCES *.cpp)

add_library(NetworkDirect STATIC ${NETWORKDIRECT_SOURCES})

target_include_directories(NetworkDirect
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}    
)