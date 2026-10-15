#pragma once
#include <algorithm>
#include <string>

inline std::string redactUris(std::string text) {
    std::size_t offset = 0;
    while ((offset = text.find("://", offset)) != std::string::npos) {
        const auto begin = offset + 3;
        const auto delimiter = text.find_first_of("/ ?#\t\r\n\"'<>\\", begin);
        const auto end = delimiter == std::string::npos ? text.size() : delimiter;
        const auto at = text.rfind('@', end);
        if (at != std::string::npos && at >= begin && at < end) {
            text.replace(begin, at - begin, "***");
            offset = begin + 4;
        } else
            offset = end;
    }
    return text;
}
