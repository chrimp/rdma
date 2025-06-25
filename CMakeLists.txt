# CMakeList.txt : CMake project for rdma, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.12)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add source to this project's executable.
project (rdma CXX)

# Set a custom output directory for the final executable.
# CMake will create this directory automatically.
# The $<CONFIG> generator expression will resolve to "Debug", "Release", etc.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/bin/$<CONFIG>")

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

add_subdirectory("examples/send_recv")
add_subdirectory("examples/read_write")

if (WIN32)
    add_subdirectory("include/Win/NetworkDirect")
    add_subdirectory("include/Win/MyND2Class")

    #target_link_libraries(rdma PRIVATE NetworkDirect MyND2Class ws2_32)
endif()

# TODO: Add tests and install targets if needed.
