#include "http/http_response.h"

#include <sstream>

namespace http {

std::string BuildSimpleOkResponse(std::string_view body, bool keep_alive) {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    oss << "Content-Type: text/plain\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}

std::string BuildSimpleErrorResponse(int status_code,
                                     std::string_view reason_phrase,
                                     std::string_view body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << ' ' << reason_phrase << "\r\n";
    oss << "Connection: close\r\n";
    oss << "Content-Type: text/plain\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}

}  // namespace http
