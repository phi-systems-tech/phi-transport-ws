cmake_minimum_required(VERSION 3.21)

project(phi-transport-ws
    VERSION 1.0.0
    DESCRIPTION "WebSocket transport plugin for phi-core"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

# Qt-free since contract 2.0.0: the plugin runs on the runtime loop core
# hands it, and WsServer speaks RFC 6455 over that loop's watches.
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(phi-transport-api 2.1.0 CONFIG QUIET)

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
    src/wsserver.cpp
    src/wsserver.h
)

target_link_libraries(phi_transport_ws
    PRIVATE
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        ${PHI_TRANSPORT_API_TARGET}
)

set_target_properties(phi_transport_ws PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/transports"
)

install(TARGETS phi_transport_ws
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/transports
)
