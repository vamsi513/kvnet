#include "protocol.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <vector>

namespace kvnet {

namespace {

std::vector<std::string> split(const std::string& line, std::size_t max_parts) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        if (parts.size() + 1 == max_parts) {  // last part keeps the rest of the line
            parts.push_back(line.substr(i));
            break;
        }
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        parts.push_back(line.substr(i, j - i));
        i = j;
    }
    return parts;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string Store::execute(const std::string& line) {
    auto p = split(line, 3);
    if (p.empty()) return "-ERR empty command\r\n";
    const std::string cmd = upper(p[0]);

    if (cmd == "PING") return "+PONG\r\n";

    if (cmd == "SET") {
        if (p.size() != 3) return "-ERR usage: SET key value\r\n";
        data_[p[1]] = p[2];
        return "+OK\r\n";
    }
    if (cmd == "GET") {
        if (p.size() != 2) return "-ERR usage: GET key\r\n";
        auto it = data_.find(p[1]);
        if (it == data_.end()) return "*-1\r\n";  // distinct from the value -1
        return "$" + it->second + "\r\n";
    }
    if (cmd == "DEL") {
        if (p.size() != 2) return "-ERR usage: DEL key\r\n";
        return data_.erase(p[1]) ? ":1\r\n" : ":0\r\n";
    }
    if (cmd == "INCR") {
        if (p.size() != 2) return "-ERR usage: INCR key\r\n";
        long long v = 0;
        auto it = data_.find(p[1]);
        if (it != data_.end()) {
            errno = 0;
            char* end = nullptr;
            v = std::strtoll(it->second.c_str(), &end, 10);
            if (errno != 0 || end == it->second.c_str() || *end != '\0')
                return "-ERR value is not an integer\r\n";
        }
        if (v == std::numeric_limits<long long>::max())
            return "-ERR value out of range\r\n";  // ++v would overflow
        ++v;
        data_[p[1]] = std::to_string(v);
        return ":" + std::to_string(v) + "\r\n";
    }
    return "-ERR unknown command\r\n";
}

}  // namespace kvnet
