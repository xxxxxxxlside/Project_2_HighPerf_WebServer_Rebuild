#include "net/dynamic_buffer.h"

#include <stdexcept>

namespace net {

// [Week1 Day4] Begin: 动态缓冲区实现。

// 把新读到的字节追加到缓冲区末尾。
// 这样即使一个请求头被拆成多次 read() 才收全，也能重新拼回完整内容。
void DynamicBuffer::Append(const char* data, std::size_t size) {
    if (data == nullptr && size != 0) {
        throw std::invalid_argument("data must not be null when size is non-zero");
    }
    storage_.append(data, size);
}

// 返回当前还留在缓冲区里的字节数。
std::size_t DynamicBuffer::size() const noexcept {
    return storage_.size();
}

// 判断缓冲区是否为空。
bool DynamicBuffer::empty() const noexcept {
    return storage_.empty();
}

// 只要找到了 "\r\n\r\n"，就说明至少有一个完整请求头已经进入缓冲区。
bool DynamicBuffer::HasCompleteHeader() const {
    return storage_.find("\r\n\r\n") != std::string::npos;
}

// 取出一个完整请求头。
// 这里的核心点是：
// - 如果一次 read() 读到了多个请求头，就可以循环调用这个函数逐个取出
// - 如果请求头只收了一半，这个函数不会被调用，数据会继续留在缓冲区等待下次 read()
std::string DynamicBuffer::PopNextHeader() {
    const std::size_t header_end = storage_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::logic_error("no complete header in buffer");
    }

    const std::size_t header_size = header_end + 4;
    std::string header = storage_.substr(0, header_size);
    storage_.erase(0, header_size);
    return header;
}

// [Week1 Day4] End

}  // namespace net
