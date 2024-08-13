cmake_minimum_required(VERSION 3.21)
project(messenger)

set(CMAKE_CXX_STANDARD 23)

find_package( Boost 1.74 COMPONENTS program_options REQUIRED )
include_directories( ${Boost_INCLUDE_DIR} )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpqxx -lpq")

set(PQXX /usr/local/include/pqxx)

find_library(PQXX_LIB pqxx)
find_library(PQ_LIB pq)

add_executable(run_server server_src/server.cpp)
add_executable(run_client client_src/client.cpp)


target_link_libraries( run_server LINK_PUBLIC ${Boost_LIBRARIES} ${PQXX_LIB} ${PQ_LIB})
target_link_libraries( run_client LINK_PUBLIC ${Boost_LIBRARIES})

