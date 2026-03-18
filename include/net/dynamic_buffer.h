#pragma once

#include <cstddef>
#include <string>

namespace net {

// DynamicBuffer 是最小应用层输入缓冲区。
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
    // 清空整个缓冲区。
    // 关闭连接时会用它把仍然残留的输入字节一次性丢弃。
    void Clear() noexcept;

    // 返回第一个 "\r\n\r\n" 的起始位置。
    // 如果还没有找到完整请求头，则返回 std::string::npos。
    [[nodiscard]] std::size_t FindHeaderEnd() const noexcept;
    // 判断当前缓冲区里是否已经出现了一个完整的 HTTP 请求头。
    [[nodiscard]] bool HasCompleteHeader() const;
    // 判断“当前正在寻找的这个请求头”是否已经超过限制。
    // 这里既覆盖“还没找到边界但缓冲区已经太大”的情况，
    // 也覆盖“找到了边界，但这个请求头本身就超过限制”的情况。
    [[nodiscard]] bool HeaderExceedsLimit(std::size_t limit) const;

    // 取出下一个完整请求头，并从缓冲区中删除它。
    // 返回值会包含结尾的 "\r\n\r\n"。
    std::string PopNextHeader();

private:
    // 真正保存未处理字节的底层容器。
    std::string storage_;
};

}  // namespace net
