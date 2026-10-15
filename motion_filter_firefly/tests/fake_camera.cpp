// Test executable for launcher lifecycle; never used to validate decoder/FPS.
#include "config.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
std::atomic<bool> stopped{false};

void onStop(int) {
    stopped = true;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 &&
            (std::string(argv[1]) == "--camera-list" || std::string(argv[1]) == "--output-root" ||
             std::string(argv[1]) == "--runtime-settings")) {
            const auto cfg = loadConfig(argv[2]);
            const auto cameras = loadCameras(argv[2]);
            if (std::string(argv[1]) == "--output-root")
                std::cout << cfg.outputRoot << '\n';
            else if (std::string(argv[1]) == "--runtime-settings")
                writeRuntimeSettings(cfg, std::cout);
            else
                for (const auto& c : cameras)
                    std::cout << c.id << '\t' << c.urlEnv << '\t' << c.codec << '\t' << c.gopHintSec
                              << '\n';
            return 0;
        }
        if (argc != 6)
            return 1;
        std::signal(SIGINT, onStop);
        std::signal(SIGTERM, onStop);
        const char* dir = std::getenv("MOTION_TEST_ARGS_DIR");
        if (!dir)
            return 1;
        {
            std::ofstream f(std::filesystem::path(dir) / (std::string(argv[3]) + ".args"));
            f << argv[1] << '\n' << getpid() << '\n';
        }
        if (std::string(argv[1]).find("/fatal") != std::string::npos)
            return 2;
        unsigned tick = 0;
        while (!stopped) {
            ++tick;
            std::cout
                << "[PERF] wall_sec=" << tick * .1
                << " dt_sec=0.1 frames=2 analysis_fps=20 decode_fps=20 pull_fps=20 sample_skips=0 "
                   "wait_ms=1 map_ms=1 rga_ms=1 process_ms=1 event_ms=0 cpu_pct=1\n"
                << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
