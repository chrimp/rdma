cmake_minimum_required(VERSION 3.12)

file(GLOB NDSESSION_SOURCES src/*.cpp)
file(GLOB NDSESSION_HEADERS include/*.hpp)

# Create a library from MyNDSession
add_library(NDSession STATIC ${NDSESSION_SOURCES})

# Set C++20 for this library
set_property(TARGET NDSession PROPERTY CXX_STANDARD 20)
set_property(TARGET NDSession PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(NDSession 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
if (WIN32)
    target_link_libraries(NDSession 
        PUBLIC 
            NetworkDirect  # If MyNDSession depends on NetworkDirect
        PRIVATE
            ws2_32
    )
endif()

# Set compile definitions if needed
target_compile_definitions(NDSession PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)