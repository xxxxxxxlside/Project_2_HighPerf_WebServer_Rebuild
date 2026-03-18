#include "http/http_parser.h"

#include <cctype>
#include <stdexcept>

namespace http {

ParseError HttpParser::ParseRequestHeader(std::string_view header, HttpRequest* request) const {
    if (request == nullptr) {
        return ParseError::kBadRequest;
    }

    request->Clear();

    const std::size_t first_line_end = header.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        return ParseError::kBadRequest;
    }

    const ParseError line_result = ParseRequestLine(header.substr(0, first_line_end), request);
    if (line_result != ParseError::kNone) {
        return line_result;
    }

    std::size_t cursor = first_line_end + 2;
    while (cursor < header.size()) {
        const std::size_t next_line_end = header.find("\r\n", cursor);
        if (next_line_end == std::string_view::npos) {
            return ParseError::kBadRequest;
        }

        const std::string_view line = header.substr(cursor, next_line_end - cursor);
        if (line.empty()) {
            break;
        }

        const ParseError header_result = ParseHeaderLine(line, request);
        if (header_result != ParseError::kNone) {
            return header_result;
        }

        cursor = next_line_end + 2;
    }

    if (request->is_chunked_transfer_encoding()) {
        return ParseError::kNotImplementedChunked;
    }

    if (request->method() == HttpMethod::kPost && !request->has_content_length()) {
        return ParseError::kLengthRequired;
    }

    return ParseError::kNone;
}

ParseError HttpParser::ParseRequestLine(std::string_view line, HttpRequest* request) const {
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return ParseError::kBadRequest;
    }

    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return ParseError::kBadRequest;
    }

    const std::string_view method_token = line.substr(0, first_space);
    const std::string_view uri = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);

    const HttpMethod method = ParseMethodToken(method_token);
    if (method == HttpMethod::kUnknown || uri.empty() || !IsSupportedVersion(version)) {
        return ParseError::kBadRequest;
    }

    request->SetMethod(method);
    request->SetUri(std::string(uri));
    request->SetVersion(std::string(version));
    return ParseError::kNone;
}

ParseError HttpParser::ParseHeaderLine(std::string_view line, HttpRequest* request) const {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return ParseError::kBadRequest;
    }

    const std::string key = LowercaseCopy(Trim(line.substr(0, colon)));
    const std::string value = Trim(line.substr(colon + 1));
    if (key.empty()) {
        return ParseError::kBadRequest;
    }

    request->AddHeader(key, value);

    if (key == "transfer-encoding" && ContainsChunkedToken(value)) {
        request->MarkChunkedTransferEncoding();
    }

    if (key == "content-length") {
        std::size_t content_length = 0;
        if (!ParseContentLengthValue(value, &content_length)) {
            return ParseError::kBadRequest;
        }
        request->SetContentLength(content_length);
    }

    return ParseError::kNone;
}

HttpMethod HttpParser::ParseMethodToken(std::string_view token) noexcept {
    if (token == "GET") {
        return HttpMethod::kGet;
    }
    if (token == "POST") {
        return HttpMethod::kPost;
    }
    return HttpMethod::kUnknown;
}

bool HttpParser::IsSupportedVersion(std::string_view version) noexcept {
    return version == "HTTP/1.1" || version == "HTTP/1.0";
}

std::string HttpParser::Trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::string HttpParser::LowercaseCopy(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool HttpParser::ContainsChunkedToken(std::string_view value) {
    const std::string lower = LowercaseCopy(value);
    return lower.find("chunked") != std::string::npos;
}

bool HttpParser::ParseContentLengthValue(std::string_view value, std::size_t* content_length) {
    if (content_length == nullptr) {
        return false;
    }

    const std::string trimmed = Trim(value);
    if (trimmed.empty()) {
        return false;
    }

    std::size_t parsed = 0;
    try {
        *content_length = std::stoull(trimmed, &parsed);
    } catch (const std::exception&) {
        return false;
    }
    return parsed == trimmed.size();
}

const char* ToString(ParseError error) noexcept {
    switch (error) {
    case ParseError::kNone:
        return "none";
    case ParseError::kBadRequest:
        return "bad request";
    case ParseError::kLengthRequired:
        return "length required";
    case ParseError::kNotImplementedChunked:
        return "chunked transfer encoding is not supported";
    default:
        return "unknown parse error";
    }
}

}  // namespace http
