#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace http {

// 当前阶段只支持文档里明确提到的 GET / POST。
enum class HttpMethod {
    kUnknown,
    kGet,
    kPost,
};

// HttpRequest 用来承载 Day5 解析出来的最小请求头信息。
// 这里只保存：
// 1. Request Line
// 2. Header 键值
// 3. Content-Length
// 4. 是否出现了 Chunked
class HttpRequest {
public:
    // 清空上一次解析结果，方便复用对象。
    void Clear();

    void SetMethod(HttpMethod method) noexcept;
    void SetUri(std::string uri);
    void SetVersion(std::string version);
    void AddHeader(std::string key, std::string value);
    void SetContentLength(std::size_t content_length) noexcept;
    void MarkChunkedTransferEncoding() noexcept;

    [[nodiscard]] HttpMethod method() const noexcept;
    [[nodiscard]] const std::string& uri() const noexcept;
    [[nodiscard]] const std::string& version() const noexcept;
    [[nodiscard]] const std::unordered_map<std::string, std::string>& headers() const noexcept;
    [[nodiscard]] bool has_content_length() const noexcept;
    [[nodiscard]] std::size_t content_length() const noexcept;
    [[nodiscard]] bool is_chunked_transfer_encoding() const noexcept;

    // 按 header 名查找值。
    // 这里假设 header 名已经在 parser 里统一转成小写。
    [[nodiscard]] bool HasHeader(std::string_view key) const;
    [[nodiscard]] const std::string* FindHeader(std::string_view key) const noexcept;

private:
    HttpMethod method_ = HttpMethod::kUnknown;
    std::string uri_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::optional<std::size_t> content_length_;
    bool chunked_transfer_encoding_ = false;
};

// 把方法枚举转成日志友好的字符串。
const char* ToString(HttpMethod method) noexcept;

}  // namespace http
