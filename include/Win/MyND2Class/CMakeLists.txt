cmake_minimum_required(VERSION 3.12)

file(GLOB MYND2CLASS_SOURCES src/*.cpp)
file(GLOB MYND2CLASS_HEADERS include/*.hpp)

# Create a library from MyNDSession
add_library(MyND2Class STATIC ${MYND2CLASS_SOURCES})

# Set C++20 for this library
set_property(TARGET MyND2Class PROPERTY CXX_STANDARD 20)
set_property(TARGET MyND2Class PROPERTY CXX_STANDARD_REQUIRED ON)

# Public include directories - these will be propagated to targets that link to MyNDSession
target_include_directories(MyND2Class 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link dependencies
if (WIN32)
    target_link_libraries(MyND2Class 
        PUBLIC 
            NetworkDirect  # If MyNDSession depends on NetworkDirect
        PRIVATE
            ws2_32
    )
endif()

# Set compile definitions if needed
target_compile_definitions(MyND2Class PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)