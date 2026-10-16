#pragma once
#include <string>

struct Config {
    std::string outputRoot;

    // Operational policy (adjustable, not motion thresholds).
    int analysisMaxWidth;
    int analysisMaxHeight;
    int analysisAllowUpscale;
    double analysisTargetFps;
    double fallbackFps;
    double startupWarmupSec;
    double eventPreSec;
    double eventPostSec;

    // Event policy: minimum unexplained foreground as a fraction of the
    // actual analysis frame area. This is the single explicit event threshold.
    double eventMinValidRatio;

    // TAWL research ratios.
    double tawlTemporalFpsRatio;
    double tawlWidthFrameRatio;

    // WSL/offline recorder.
    int offlineRecordEnabled;
    std::string offlineFourcc;
    int offlineWriterQueueMaxMessages;
    double recordSafetySec;

    // Full-video L1/L2/L3 evidence masks (WSL test only).
    int fullMaskEnabled;
    std::string fullMaskFourcc;

    // L1/L2/L3 evidence clips around events.
    int validationEnabled;
    double validationRingSec;
    int validationQueueMaxMessages;
    std::string validationFourcc;

    // Optional CSV telemetry.
    int frameStatsEnabled;
    double statsIntervalSec;

    // Firefly-only fields; kept in the shared policy so WSL/Firefly use one schema.
    std::string decoderElement;
    int rtspLatencyMs;
    int rtspUseTcp;
    int analysisDropOldFrames;
    int analysisMaxBuffers;
    int encodedMaxBuffers;
    int recordEnabled;
    double recordRingSec;
    int writerQueueMaxMessages;
};

Config loadConfig(const std::string& path);
