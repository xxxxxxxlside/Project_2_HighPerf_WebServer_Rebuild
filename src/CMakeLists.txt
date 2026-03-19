set(WEBSERVER_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

add_library(webserver_net STATIC
    net/socket_utils.cpp
    net/dynamic_buffer.cpp
    net/epoll_poller.cpp
)
target_include_directories(webserver_net PUBLIC "${WEBSERVER_INCLUDE_DIR}")
target_link_libraries(webserver_net PUBLIC webserver_options)

add_library(webserver_http STATIC
    http/http_request.cpp
    http/http_parser.cpp
    http/http_response.cpp
)
target_include_directories(webserver_http PUBLIC "${WEBSERVER_INCLUDE_DIR}")
target_link_libraries(webserver_http PUBLIC webserver_options)

add_library(webserver_core STATIC
    core/server.cpp
)
target_include_directories(webserver_core PUBLIC "${WEBSERVER_INCLUDE_DIR}")
target_link_libraries(webserver_core
    PUBLIC
        webserver_http
        webserver_net
        webserver_options
)

add_executable(webserver
    main.cpp
)
target_link_libraries(webserver
    PRIVATE
        webserver_core
        webserver_http
        webserver_net
        webserver_options
)
set_target_properties(webserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Day4 的核心是给当前闭环补一条可重复执行的 sanitizer 自测入口。
# 这里先用 Python 脚本启动服务、压一小段已有场景，并检查 metrics 是否仍然守恒。
if(BUILD_TESTING)
    add_test(
        NAME webserver_sanitizer_smoke
        COMMAND
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/sanitizer_smoke.py
            --server-path $<TARGET_FILE:webserver>
            --duration-sec 3
    )
    set_tests_properties(webserver_sanitizer_smoke PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
endif()
