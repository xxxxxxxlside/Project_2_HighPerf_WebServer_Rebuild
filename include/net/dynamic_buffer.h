#pragma once

#include <cstddef>
#include <string>

namespace net {

// DynamicBuffer 是 Week1 Day4 新增的最小应用层输入缓冲区。
// 它的作用是把多次 read() 读到的数据先暂存起来，
// 然后再按应用层边界去切分完整请求头。
//
// 这里使用 std::string 作为底层容器，原因是：
// 1. 它本身就是可动态扩容的连续内存
// 2. append / erase / find 这些操作足够支持当前阶段需求
// 3. 对初学者来说更容易理解和调试
class DynamicBuffer {
public:
    // 追加一段新读到的数据。
    void Append(const char* data, std::size_t size);

    // 返回当前缓冲区里还没被消费的数据总字节数。
    [[nodiscard]] std::size_t size() const noexcept;
    // 判断当前缓冲区是否为空。
    [[nodiscard]] bool empty() const noexcept;

    // 判断当前缓冲区里是否已经出现了一个完整的 HTTP 请求头。
    // Day4 当前只按 "\r\n\r\n" 这个边界判断是否完整。
    [[nodiscard]] bool HasCompleteHeader() const;

    // 取出下一个完整请求头，并从缓冲区中删除它。
    // 返回值会包含结尾的 "\r\n\r\n"。
    std::string PopNextHeader();

private:
    // 真正保存未处理字节的底层容器。
    std::string storage_;
};

}  // namespace net
