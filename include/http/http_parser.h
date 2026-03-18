#pragma once

#include <string>
#include <string_view>

#include "http/http_request.h"

namespace http {

// ParseError 表示 Day5 解析 / 协议检查阶段的结果。
enum class ParseError {
    kNone,
    kBadRequest,
    kLengthRequired,
    kNotImplementedChunked,
};

// HttpParser 负责 Day5 的最小请求头解析。
// 当前只做：
// 1. 解析 Request Line
// 2. 解析 Headers
// 3. 提取 Content-Length
// 4. 检查 Transfer-Encoding: chunked
// 5. 检查 POST 是否缺少 Content-Length
class HttpParser {
public:
    // 解析一个完整请求头。
    // 如果成功，返回 kNone，并把结果写入 request。
    ParseError ParseRequestHeader(std::string_view header, HttpRequest* request) const;

private:
    // 解析第一行 "METHOD URI VERSION"。
    ParseError ParseRequestLine(std::string_view line, HttpRequest* request) const;
    // 解析一个普通 Header 行。
    ParseError ParseHeaderLine(std::string_view line, HttpRequest* request) const;

    static HttpMethod ParseMethodToken(std::string_view token) noexcept;
    static bool IsSupportedVersion(std::string_view version) noexcept;
    static std::string Trim(std::string_view text);
    static std::string LowercaseCopy(std::string_view text);
    static bool ContainsChunkedToken(std::string_view value);
    static bool ParseContentLengthValue(std::string_view value, std::size_t* content_length);
};

// 把解析错误转成日志和错误响应都能复用的原因短语。
const char* ToString(ParseError error) noexcept;

}  // namespace http
