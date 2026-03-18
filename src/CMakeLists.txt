set(WEBSERVER_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

add_library(webserver_net STATIC
    net/socket_utils.cpp
)
target_include_directories(webserver_net PUBLIC "${WEBSERVER_INCLUDE_DIR}")
target_link_libraries(webserver_net PUBLIC webserver_options)

add_library(webserver_core STATIC
    core/server.cpp
)
target_include_directories(webserver_core PUBLIC "${WEBSERVER_INCLUDE_DIR}")
target_link_libraries(webserver_core
    PUBLIC
        webserver_net
        webserver_options
)

add_executable(webserver
    main.cpp
)
target_link_libraries(webserver
    PRIVATE
        webserver_core
        webserver_net
        webserver_options
)
set_target_properties(webserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
