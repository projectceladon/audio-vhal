set(TARGET_OS "linux")
set(CMAKE_TOOLCHAIN_FILE "clang.toolchain.cmake")
set(UNIT_TEST 1)

cmake_minimum_required( VERSION 2.6 )
project(HGC)

set(HGC_VERION_MAJOR 0)
set(HGC_VERION_MINOR 1)
set(HGC_VERION_PATCH ${PATCH})

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -fno-exceptions -Wno-deprecated  -DOS_UBUNTU=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -fno-exceptions -Wno-deprecated -DOS_UBUNTU=1")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")

set(EXECUTABLE_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/bin/${TARGET_OS}")
set(LIBRARY_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/${TARGET_OS}")

include_directories( )

find_library(PTHREAD_LIBRARY pthread "pthread library")

link_directories()

add_executable(host_audio_out_client
  src/host_audio_out_client.c
)
target_link_libraries(host_audio_out_client
  pthread
)

add_executable(host_audio_in_client
  src/host_audio_in_client.c
)
target_link_libraries(host_audio_in_client
  pthread
)

add_executable(host_audio_in_client_unix
  src/host_audio_in_client_unix.c
)
target_link_libraries(host_audio_in_client_unix
  pthread
)

