cmake_minimum_required(VERSION 3.10)

project(sandbox VERSION 1.0)

#set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_PREFIX_PATH "/usr/lib/x86_64-linux-gnu")

find_package (glog 0.7.0 REQUIRED)

set(CMAKE_BUILD_TYPE Release)
#set(CMAKE_PREFIX_PATH "/usr/lib/x86_64-linux-gnu/")

add_library(haldendb_compiler_flags INTERFACE)
target_compile_features(haldendb_compiler_flags  INTERFACE cxx_std_20)

#configure_file(TutorialConfig.h.in TutorialConfig.h)

add_compile_definitions(__TRACK_CACHE_FOOTPRINT__)
add_compile_definitions(__CONCURRENT__)
add_compile_definitions(__TREE_WITH_CACHE__)
add_compile_definitions(__VALIDITY_CHECK__)

add_subdirectory(../libcache ${PROJECT_BINARY_DIR}/libcache)
add_subdirectory(../libbtree ${PROJECT_BINARY_DIR}/libbtree)

set(CMAKE_CXX_COMPILER g++-11)

#add_executable(sandbox sandbox.cpp)
add_executable(TestBEpsilonTree TestBEpsilonTree.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPMEM REQUIRED libpmem)

target_link_libraries(sandbox PRIVATE ${LIBPMEM_LIBRARIES})
target_include_directories(sandbox PRIVATE ${LIBPMEM_INCLUDE_DIRS})
#add_executable(sandbox sandbox.cpp)

set_target_properties(sandbox PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

#target_compile_options(sandbox PRIVATE
#    -ggdb3
#    -O0
#    -g3
#    -Wall)

target_link_libraries(sandbox PUBLIC libcache haldendb_compiler_flags)
target_link_libraries(sandbox PUBLIC libbtree haldendb_compiler_flags)
target_link_libraries (sandbox PUBLIC glog::glog)

target_include_directories(sandbox PUBLIC
                           "${PROJECT_BINARY_DIR}"
                           "${PROJECT_SOURCE_DIR}/../libcache"
                           "${PROJECT_SOURCE_DIR}/../libbtree"
                           )
