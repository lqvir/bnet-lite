cmake_minimum_required(VERSION 3.16)
project(boost_tcp_net VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(BNET_IS_TOP_LEVEL ON)
else()
    set(BNET_IS_TOP_LEVEL OFF)
endif()

option(BNET_BUILD_EXAMPLES "Build boost_tcp_net example binaries" ${BNET_IS_TOP_LEVEL})

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(ZLIB QUIET)

add_library(boost_tcp_net
    src/IOService.cpp
    src/Serialization.cpp
    src/Session.cpp
    src/Channel.cpp
)
add_library(boost_tcp_net::boost_tcp_net ALIAS boost_tcp_net)

target_compile_features(boost_tcp_net PUBLIC cxx_std_17)

target_include_directories(boost_tcp_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(boost_tcp_net PUBLIC
    Boost::system
    Threads::Threads
)

if(ZLIB_FOUND)
    target_compile_definitions(boost_tcp_net PUBLIC BNET_USE_ZLIB=1)
    target_link_libraries(boost_tcp_net PUBLIC ZLIB::ZLIB)
endif()

set(BNET_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/boost_tcp_net")

install(TARGETS boost_tcp_net
    EXPORT boost_tcp_netTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT boost_tcp_netTargets
    FILE boost_tcp_netTargets.cmake
    NAMESPACE boost_tcp_net::
    DESTINATION ${BNET_INSTALL_CMAKEDIR}
)

set(BNET_WITH_ZLIB OFF)
if(ZLIB_FOUND)
    set(BNET_WITH_ZLIB ON)
endif()

configure_package_config_file(
    cmake/boost_tcp_netConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/boost_tcp_netConfig.cmake
    INSTALL_DESTINATION ${BNET_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/boost_tcp_netConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/boost_tcp_netConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/boost_tcp_netConfigVersion.cmake
    DESTINATION ${BNET_INSTALL_CMAKEDIR}
)

if(BNET_BUILD_EXAMPLES)
    add_executable(tcp_demo examples/tcp_demo.cpp)
    target_link_libraries(tcp_demo PRIVATE boost_tcp_net::boost_tcp_net)

    add_executable(local_demo examples/local_demo.cpp)
    target_link_libraries(local_demo PRIVATE boost_tcp_net::boost_tcp_net)
endif()
