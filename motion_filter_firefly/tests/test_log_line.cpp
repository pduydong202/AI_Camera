#include "log_line.hpp"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main() {
    try {
        std::ostringstream output;
        std::vector<std::thread> threads;
        constexpr int workers = 8, records = 1000;
        for (int worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                for (int record = 0; record < records; ++record) {
                    LogLine(output) << "worker=" << worker << " record=" << record
                                    << " payload=" << std::string(256, 'a' + worker) << '\n';
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        std::set<std::string> lines;
        std::istringstream input(output.str());
        for (std::string line; std::getline(input, line);)
            lines.insert(line);
        if (lines.size() != workers * records)
            throw std::runtime_error("Lost or interleaved records");
        for (int worker = 0; worker < workers; ++worker) {
            for (int record = 0; record < records; ++record) {
                const auto expected = "worker=" + std::to_string(worker) +
                                      " record=" + std::to_string(record) +
                                      " payload=" + std::string(256, 'a' + worker);
                if (!lines.count(expected))
                    throw std::runtime_error("Log fields were interleaved");
            }
        }
        std::cout << "PASS: 8000 concurrent application log records intact\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
