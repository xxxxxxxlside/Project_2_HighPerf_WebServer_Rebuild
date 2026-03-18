#pragma once

#include <string>
#include <string_view>

namespace http {

// 为合法 GET/POST 构造一个最小 200 OK 文本响应。
// 当前故意保持简单：
// - 固定纯文本 body
// - 固定 Connection: close
// - 不引入路由表、模板系统或 keep-alive
std::string BuildSimpleOkResponse(std::string_view body);

// BuildSimpleErrorResponse 用最直接的方式拼一个小的错误响应。
// 当前只服务于 Day5 的协议防御分支，例如 431 / 411 / 501。
std::string BuildSimpleErrorResponse(int status_code,
                                     std::string_view reason_phrase,
                                     std::string_view body);

}  // namespace http
