#pragma once
#include <charconv>
#include <sched.h>
#include <stdexcept>
#include <string>

inline cpu_set_t parseCpuList(const std::string& text) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (text.empty())
        throw std::runtime_error("Empty CPU list");
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto comma = text.find(',', offset);
        const auto item =
            text.substr(offset, comma == std::string::npos ? text.size() - offset : comma - offset);
        const auto dash = item.find('-');
        auto number = [](const std::string& s) {
            int value = -1;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
            if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() || value < 0 ||
                value >= CPU_SETSIZE)
                throw std::runtime_error("Invalid main_cpu_list");
            return value;
        };
        const int first = number(item.substr(0, dash));
        const int last = dash == std::string::npos ? first : number(item.substr(dash + 1));
        if (last < first)
            throw std::runtime_error("Invalid CPU range");
        for (int cpu = first; cpu <= last; ++cpu)
            CPU_SET(cpu, &mask);
        if (comma == std::string::npos)
            break;
        offset = comma + 1;
        if (offset == text.size())
            throw std::runtime_error("Trailing comma in main_cpu_list");
    }
    return mask;
}

inline void pinMainThread(const std::string& list) {
    if (list.empty())
        return;
    const auto requested = parseCpuList(list);
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        throw std::runtime_error("Cannot read thread CPU affinity");
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &requested) && !CPU_ISSET(cpu, &allowed))
            throw std::runtime_error("Requested main CPU is unavailable: " + std::to_string(cpu));
    if (sched_setaffinity(0, sizeof(requested), &requested) != 0)
        throw std::runtime_error("Cannot set main thread CPU affinity");
}
