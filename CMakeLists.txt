cmake_minimum_required(VERSION 3.21)

project(phi-transport-ws
    VERSION 1.0.0
    DESCRIPTION "WebSocket transport plugin for phi-core"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core WebSockets)
find_package(phi-transport-api CONFIG QUIET)

if(NOT TARGET phicore::transport-api AND NOT TARGET phi_transport_api)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api/CMakeLists.txt")
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/phi-transport-api"
        )
    endif()
endif()

if(TARGET phicore::transport-api)
    set(PHI_TRANSPORT_API_TARGET phicore::transport-api)
elseif(TARGET phi_transport_api)
    set(PHI_TRANSPORT_API_TARGET phi_transport_api)
else()
    message(FATAL_ERROR "phi-transport-api target not found. Install phi-transport-api or place repo next to this project.")
endif()

add_library(phi_transport_ws MODULE
    src/wstransport.cpp
    src/wstransport.h
)

target_link_libraries(phi_transport_ws
    PRIVATE
        Qt6::Core
        Qt6::WebSockets
        ${PHI_TRANSPORT_API_TARGET}
)

set_target_properties(phi_transport_ws PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/transports"
)

install(TARGETS phi_transport_ws
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/transports
)
