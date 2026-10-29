#include "recorder_fixture.hpp"

int main(int argc, char** argv) {
    try {
        check(argc == 3, "Usage: test_recorder fixture.mp4 output_dir");
        gst_init(&argc, &argv);
        const auto packets = readPackets(argv[1]);
        const fs::path root = argv[2];
        fs::create_directories(root);
        Config cfg{};
        cfg.outputRoot = (root / "good").string();
        cfg.writerQueueMaxMessages = 4;
        {
            AsyncClipWriter writer(cfg);
            writer.start();
            const auto middle = packets.begin() + static_cast<std::ptrdiff_t>(packets.size() / 2);
            check(writer.beginClip(1, "h264", {packets.begin(), middle}), "beginClip rejected");
            for (auto it = middle; it != packets.end(); ++it)
                check(writer.pushPacket(1, *it), "Packet rejected");
            check(writer.endClip(1), "Stop rejected");
            writer.close();
            check(!writer.hasError(), writer.errorMessage().c_str());
        }
        const auto final = root / "good/clips/event_000001.mp4";
        check(fs::exists(final) && !fs::exists(final.string() + ".part"),
              "Clip not finalized atomically");
        const auto written = readPackets(final);
        check(written.size() == packets.size(), "Packet count changed");
        for (std::size_t i = 0; i < packets.size(); ++i) {
            check(written[i]->ptsNs == packets[i]->ptsNs - packets[0]->ptsNs, "PTS changed");
            check(written[i]->dtsNs == packets[i]->dtsNs - packets[0]->dtsNs, "DTS changed");
            const auto a = pictures(packets[i]->data), b = pictures(written[i]->data);
            check(!a.empty() && a == b, "Compressed picture payload changed");
        }
        // A valid one-RTP-tick gap used to collapse to duplicate PTS in MP4.
        auto tinyPackets = packets;
        tinyPackets[4] = std::make_shared<EncodedPacket>(*packets[4]);
        tinyPackets[4]->ptsNs = tinyPackets[3]->ptsNs + 11000;
        tinyPackets[4]->dtsNs = tinyPackets[3]->dtsNs + 11000;
        cfg.outputRoot = (root / "good_tiny").string();
        {
            AsyncClipWriter writer(cfg);
            writer.start();
            check(writer.beginClip(1, "h264", tinyPackets), "tiny begin rejected");
            check(writer.endClip(1), "tiny stop rejected");
            writer.close();
            check(!writer.hasError(), writer.errorMessage().c_str());
        }
        const auto tinyWritten = readPackets(root / "good_tiny/clips/event_000001.mp4");
        check(tinyWritten.size() == tinyPackets.size(), "tiny packet count changed");
        for (std::size_t i=0; i<tinyPackets.size(); ++i) {
            check(tinyWritten[i]->ptsNs == tinyPackets[i]->ptsNs - tinyPackets[0]->ptsNs,
                  "tiny PTS changed");
            check(tinyWritten[i]->dtsNs == tinyPackets[i]->dtsNs - tinyPackets[0]->dtsNs,
                  "tiny DTS changed");
            check(pictures(tinyWritten[i]->data) == pictures(tinyPackets[i]->data),
                  "tiny compressed picture changed");
        }
        cfg.outputRoot = (root / "bad").string();
        fs::create_directories(root / "bad/clips/event_000001.mp4.part");
        {
            AsyncClipWriter writer(cfg);
            writer.start();
            writer.beginClip(1, "h264", packets);
            writer.endClip(1);
            writer.close();
            check(writer.hasError(), "Filesink failure was hidden");
        }
        check(!fs::exists(root / "bad/clips/event_000001.mp4"),
              "Failed clip was published as ready");
        std::cout << "PASS: " << written.size()
                  << " packets, compressed pictures/PTS/DTS preserved; completed rename and "
                     "writer-error propagation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
