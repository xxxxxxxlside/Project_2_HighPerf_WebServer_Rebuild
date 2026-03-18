#include "http/http_request.h"

#include <utility>

namespace http {

void HttpRequest::Clear() {
    method_ = HttpMethod::kUnknown;
    uri_.clear();
    version_.clear();
    headers_.clear();
    content_length_.reset();
    chunked_transfer_encoding_ = false;
}

void HttpRequest::SetMethod(HttpMethod method) noexcept {
    method_ = method;
}

void HttpRequest::SetUri(std::string uri) {
    uri_ = std::move(uri);
}

void HttpRequest::SetVersion(std::string version) {
    version_ = std::move(version);
}

void HttpRequest::AddHeader(std::string key, std::string value) {
    headers_.insert_or_assign(std::move(key), std::move(value));
}

void HttpRequest::SetContentLength(std::size_t content_length) noexcept {
    content_length_ = content_length;
}

void HttpRequest::MarkChunkedTransferEncoding() noexcept {
    chunked_transfer_encoding_ = true;
}

HttpMethod HttpRequest::method() const noexcept {
    return method_;
}

const std::string& HttpRequest::uri() const noexcept {
    return uri_;
}

const std::string& HttpRequest::version() const noexcept {
    return version_;
}

const std::unordered_map<std::string, std::string>& HttpRequest::headers() const noexcept {
    return headers_;
}

bool HttpRequest::has_content_length() const noexcept {
    return content_length_.has_value();
}

std::size_t HttpRequest::content_length() const noexcept {
    return content_length_.value_or(0);
}

bool HttpRequest::is_chunked_transfer_encoding() const noexcept {
    return chunked_transfer_encoding_;
}

bool HttpRequest::HasHeader(std::string_view key) const {
    return FindHeader(key) != nullptr;
}

const std::string* HttpRequest::FindHeader(std::string_view key) const noexcept {
    const auto it = headers_.find(std::string(key));
    if (it == headers_.end()) {
        return nullptr;
    }
    return &it->second;
}

const char* ToString(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::kGet:
        return "GET";
    case HttpMethod::kPost:
        return "POST";
    case HttpMethod::kUnknown:
    default:
        return "UNKNOWN";
    }
}

}  // namespace http
