#include "main_affinity.hpp"
#include <future>
#include <iostream>
#include <thread>

void check(bool v, const char* why) {
    if (!v)
        throw std::runtime_error(why);
}

bool equal(const cpu_set_t& a, const cpu_set_t& b) {
    return CPU_EQUAL(&a, &b) != 0;
}

int main() {
    try {
        const auto parsed = parseCpuList("0,4-7");
        check(CPU_COUNT(&parsed) == 5 && CPU_ISSET(7, &parsed), "CPU parsing failed");
        for (const std::string bad : {"", "4-3", "1,", "-1", "1,,2", "1-x", "1024", "1-2-3"}) {
            bool failed = false;
            try {
                parseCpuList(bad);
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, "Bad CPU list accepted");
        }
        cpu_set_t original;
        check(sched_getaffinity(0, sizeof(original), &original) == 0, "Cannot read affinity");
        int first = 0;
        while (first < CPU_SETSIZE && !CPU_ISSET(first, &original))
            ++first;
        std::promise<void> ready, go;
        auto waitGo = go.get_future();
        std::promise<cpu_set_t> result;
        std::thread worker([&] {
            ready.set_value();
            waitGo.wait();
            cpu_set_t m;
            sched_getaffinity(0, sizeof(m), &m);
            result.set_value(m);
        });
        ready.get_future().wait();
        pinMainThread(std::to_string(first));
        cpu_set_t current;
        sched_getaffinity(0, sizeof(current), &current);
        go.set_value();
        const auto other = result.get_future().get();
        worker.join();
        sched_setaffinity(0, sizeof(original), &original);
        check(CPU_COUNT(&current) == 1 && CPU_ISSET(first, &current), "Main not pinned");
        check(equal(other, original), "Other thread affinity changed");
        std::cout
            << "PASS: CPU list validation and main-only affinity; existing worker mask unchanged\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
