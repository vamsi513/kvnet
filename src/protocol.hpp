// Line-based protocol parser and command executor. No socket code here, so it
// can be unit tested without a network.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace kvnet {

constexpr std::size_t kMaxLine = 4096;  // longest accepted request line

class Store {
public:
    // Executes one request line (without the trailing newline) and returns
    // the reply, including its trailing "\r\n".
    std::string execute(const std::string& line);
    std::size_t size() const { return data_.size(); }

private:
    std::unordered_map<std::string, std::string> data_;
};

}  // namespace kvnet
