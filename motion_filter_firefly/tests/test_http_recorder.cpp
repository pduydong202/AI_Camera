#include "recorder_fixture.hpp"
#include "event_manager.hpp"
#include <thread>

int main(int argc, char** argv) {
    try {
        if (argc == 4 && (std::string(argv[1]) == "--verify" || std::string(argv[1]) == "--verify-event" || std::string(argv[1]) == "--verify-jitter")) {
            const bool event = std::string(argv[1]) == "--verify-event";
            const bool jitter = std::string(argv[1]) == "--verify-jitter";
            gst_init(&argc, &argv);
            auto expected = readPackets(argv[2]);
            const auto actual = readPackets(argv[3]);
            if (jitter)
                for (std::size_t i = 0; i < expected.size(); ++i)
                    if (i % 20 == 4) {
                        expected[i]->ptsNs += 500000;
                        expected[i]->dtsNs += 500000;
                    }
            if (event) {
                const auto source = expected;
                expected.clear();
                check(source.size() == 80, "event fixture must have 80 frames");
                for (int i = 20; i <= 110; ++i) {
                    auto p = std::make_shared<EncodedPacket>(*source[i % 80]);
                    p->ptsNs += (i / 80) * 4000000000LL - 1000000000LL;
                    p->dtsNs += (i / 80) * 4000000000LL - 1000000000LL;
                    expected.push_back(p);
                }
            }
            check(expected.size() == actual.size(), "remote packet count changed");
            for (std::size_t i = 0; i < expected.size(); ++i) {
                check(expected[i]->ptsNs == actual[i]->ptsNs, "remote PTS changed");
                check(expected[i]->dtsNs == actual[i]->dtsNs, "remote DTS changed");
                check(expected[i]->keyframe == actual[i]->keyframe, "remote keyframe changed");
                check(pictures(expected[i]->data) == pictures(actual[i]->data), "remote picture payload changed");
            }
            std::cout << "PASS: " << actual.size() << " remote compressed pictures/PTS/DTS/keyframes preserved\n";
            return 0;
        }
        check(argc == 6, "Usage: test_http_recorder fixture output url token good|event|reject|recovery|budget");
        gst_init(&argc, &argv);
        const auto packets = readPackets(argv[1]);
        Config cfg{};
        cfg.outputRoot = argv[2];
        cfg.recordDestination = "http";
        cfg.recordServerUrl = argv[3];
        cfg.recordToken = argv[4];
        cfg.recordDeviceId = "firefly01";
        cfg.recordCameraId = "cam01";
        cfg.recordSessionId = "test_session.001";
        cfg.writerQueueMaxMessages = 1024;
        cfg.httpTimeoutSec = 2;
        cfg.writerEosTimeoutSec = 3;
        const std::string mode = argv[5];
        if (mode == "jitter")
            for (std::size_t i = 0; i < packets.size(); ++i)
                if (i % 20 == 4) {
                    packets[i]->ptsNs += 500000;
                    packets[i]->dtsNs += 500000;
                }
        const auto started = std::chrono::steady_clock::now();
        AsyncClipWriter writer(cfg);
        writer.start();
        auto submit = [&](std::uint64_t id) {
            const auto middle = packets.begin() + static_cast<std::ptrdiff_t>(packets.size() / 2);
            check(writer.beginClip(id, "h264", {packets.begin(), middle}), "begin rejected");
            for (auto it = middle; it != packets.end(); ++it) writer.pushPacket(id, *it);
            writer.endClip(id);
        };
        if (mode == "event") {
            cfg.recordEnabled = 1;
            cfg.eventPreSec = cfg.eventPostSec = 2;
            cfg.recordRingSec = 4;
            cfg.recordSafetySec = 1;
            EventManager events(cfg);
            EncodedRecorder recorder(cfg, writer, "h264", 0);
            for (int i = 0; i < 160; ++i) {
                auto p = std::make_shared<EncodedPacket>(*packets[i % 80]);
                p->ptsNs += (i / 80) * 4000000000LL;
                p->dtsNs += (i / 80) * 4000000000LL;
                recorder.pushPacket(p);
                if (auto signal = events.process(i >= 65 && i <= 70, p->ptsNs, p->durationNs)) {
                    if (signal->type == EventSignal::Type::Start) recorder.startEvent(signal->eventId, signal->ptsNs);
                    else recorder.stopEvent(signal->eventId);
                }
            }
            recorder.finish();
        } else if (mode == "budget") {
            auto huge = std::make_shared<EncodedPacket>();
            huge->data.resize(33U * 1024U * 1024U);
            const auto begin = std::chrono::steady_clock::now();
            check(!writer.beginClip(1, "h264", {huge}), "unbounded packet accepted");
            check(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250),
                  "network enqueue blocked producer");
            submit(2);
        } else {
            submit(1);
            if (mode == "recovery") {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (writer.lostEvents() == 0 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                check(writer.lostEvents() > 0, "server failure was hidden");
                submit(2);
            }
        }
        writer.close();
        check(!writer.hasError(), "network failure killed the analysis worker");
        check(writer.sentEvents() == (mode == "reject" ? 0U : 1U), "unexpected confirmed event count");
        if (mode != "good" && mode != "event" && mode != "jitter")
            check(writer.lostEvents() >= 1, "missing lost event diagnostic");
        check(std::chrono::steady_clock::now() - started < std::chrono::seconds(12), "unbounded close");
        check(!fs::exists(cfg.outputRoot), "HTTP mode created a local video directory");
        std::cout << "PASS: HTTP " << mode << " confirmed=" << writer.sentEvents()
                  << " failure_boundaries=" << writer.lostEvents() << " no local video files\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
