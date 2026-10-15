#include "config.hpp"
#include "redact.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

void check(bool v, const char* why) {
    if (!v)
        throw std::runtime_error(why);
}

std::string replace(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line, out;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.rfind(key + ":", 0) == 0) {
            found = true;
            if (!value.empty())
                out += key + ": " + value + '\n';
        } else
            out += line + '\n';
    }
    check(found, "Fixture key not found");
    return out;
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "Usage: test_config config.yaml temporary_dir");
        const auto good = loadConfig(argv[1]);
        const auto cameras = loadCameras(argv[1]);
        check(good.analysisTargetFps > 0 && !cameras.empty(), "Valid config failed");
        std::ifstream f(argv[1]);
        std::ostringstream content;
        content << f.rdbuf();
        const auto text = content.str();
        const fs::path dir = argv[2];
        fs::create_directories(dir);
        unsigned rejected = 0;
        auto reject = [&](const std::string& value, bool camera = false) {
            const auto path = dir / ("bad" + std::to_string(rejected) + ".yaml");
            {
                std::ofstream out(path);
                out << value;
            }
            bool failed = false;
            try {
                loadConfig(path.string());
                if (camera)
                    loadCameras(path.string());
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, "Invalid config was accepted");
            ++rejected;
        };
        reject(replace(text, "analysis_target_fps", ""));
        reject(replace(text, "analysis_target_fps", ".Nan"));
        reject(replace(text, "analysis_max_width", "854.5"));
        reject(replace(text, "record_enabled", "0.5"));
        reject(replace(text, "record_enabled", "2"));
        reject(replace(text, "stats_interval_sec", "0"));
        reject(replace(text, "rtsp_retry_max_sec", "0.5"));
        reject(replace(text, "main_cpu_list", "\"4-3\""));
        reject(replace(text, "l1_history_frames", "0"));
        reject(replace(text, "l1_var_threshold", "0.0"));
        reject(replace(text, "l1_learning_rate", "1.1"));
        reject(replace(text, "l1_open_kernel_size", "2"));
        reject(replace(text, "l2_history_frames", "-1"));
        reject(replace(text, "l2_repeat_neighborhood_size", "0"));
        reject(replace(text, "l3_location_neighborhood_size", "4"));
        reject(replace(text, "component_connectivity", "6"));
        reject(replace(text, "rtsp_watchdog_poll_ms", "0"));
        reject(replace(text, "rtsp_retry_multiplier", "0.5"));
        reject(replace(text, "source_poll_timeout_ms", "20000"));
        reject(replace(text, "offline_sampling_tolerance_ms", "50.0"));
        reject(replace(
            replace(replace(text, "gst_queue_max_buffers", "0"), "gst_queue_max_bytes", "0"),
            "gst_queue_max_time_ms", "0"));
        reject(replace(text, "writer_appsrc_max_bytes", "0"));
        reject(replace(text, "writer_eos_timeout_sec", "20.0"));
        reject(replace(text, "status_interval_sec", "0"));
        reject(replace(text, "system_sample_interval_sec", "0"));
        reject(replace(text, "affinity_snapshot_after_sec", "-1"));
        reject(replace(text, "launcher_poll_ms", "11000"));
        reject(replace(text, "evidence_sample_clip_count", "-1"));
        reject(replace(text, "evidence_sample_max_mib", "-1"));
        reject(text + "typo_key: 1\n");
        reject(text + "analysis_target_fps: 20.0\n");
        const std::string duplicate = "  - { id: " + cameras[0].id +
                                      ", url_env: TEST_RTSP, codec: h264, gop_hint_sec: 0.0 }\n";
        reject(text + duplicate, true);
        reject(text + "  - { id: extra, url_env: TEST_RTSP, codec: invalid, gop_hint_sec: 0.0 }\n",
               true);
        check(redactUris("rtsp://alice:secret@host/cam") == "rtsp://***@host/cam",
              "URI credentials leaked");
        check(redactUris("rtsp://host/cam") == "rtsp://host/cam", "Plain URI changed");
        const auto masked = redactUris("one rtsp://a:secret@host/cam two rtsps://b:other@host/cam");
        check(masked.find("secret") == std::string::npos &&
                  masked.find("other") == std::string::npos,
              "Multiple credentials leaked");
        std::cout << "PASS: config/camera schema, " << rejected
                  << " invalid cases rejected, URL credentials redacted\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
