#pragma once

#include <string>
#include <string_view>

namespace http {

// BuildSimpleErrorResponse 用最直接的方式拼一个小的错误响应。
// 当前只服务于 Day5 的协议防御分支，例如 431 / 411 / 501。
std::string BuildSimpleErrorResponse(int status_code,
                                     std::string_view reason_phrase,
                                     std::string_view body);

}  // namespace http
