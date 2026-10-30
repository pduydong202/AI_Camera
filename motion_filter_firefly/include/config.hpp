#pragma once
#include <iosfwd>
#include <string>
#include <vector>

struct Config {
    std::string outputRoot;

    // Operational policy (adjustable, not motion thresholds).
    int analysisMaxWidth;
    int analysisMaxHeight;
    int analysisAllowUpscale;
    double analysisTargetFps;
    // Keep rate limiting enabled unless an explicit controlled test disables it.
    int analysisSamplingEnabled = 1;
    // Additional bounded live-rate credit, independent of decoded queue size.
    // All configuration keys are explicit in config.yaml.
    int analysisRateBurstMs = 0;
    // Optional implementation optimization; no new detection threshold.
    int analysisSparseL3Enabled = 0;
    int opencvThreads;
    std::string mainCpuList;
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

    // Explicit algorithm parameters. The shipped YAML preserves the validated
    // MOG2 defaults and 3x3 topology operators. Shadows remain disabled because
    // every downstream mask has the binary 0/255 contract.
    int l1HistoryFrames = 500;
    double l1VarThreshold = 16.0;
    double l1LearningRate = -1.0;
    int l1OpenKernelSize = 3;
    int l2HistoryFrames = 0; // 0 follows l1_history_frames, as before.
    int l2RepeatNeighborhoodSize = 3;
    int l3LocationNeighborhoodSize = 3;
    int componentConnectivity = 8;

    double recordSafetySec;

    // L1/L2/L3 evidence clips around events.
    int validationEnabled;
    double validationRingSec;
    int validationQueueMaxMessages;
    std::string validationFourcc;

    // Optional CSV telemetry.
    int frameStatsEnabled;
    double statsIntervalSec;
    // Optional wall-clock diagnostics.
    int perfStatsEnabled = 0;
    // Lossless continuous diagnostic capture; disabled for normal operation.
    int l2CaptureEnabled = 0;

    // Firefly transport and decode policy.
    std::string decoderElement;
    int rtspLatencyMs;
    // GStreamer rtspsrc enum: 0=RTP timestamps (default), 3=auto.
    // Operational timestamp policy, separate from detection thresholds.
    int rtspBufferMode = 0;
    int rtspDropOnLatency;
    int rtspUseTcp;
    // Process recovery applies to RTSP inputs only.
    int rtspReconnectEnabled = 0;
    double rtspStartupTimeoutSec = 20.0;
    double rtspStallTimeoutSec = 15.0;
    double rtspShutdownTimeoutSec = 20.0;
    double rtspRetryInitialSec = 1.0;
    double rtspRetryMaxSec = 30.0;
    double rtspRetryResetSec = 30.0;
    int rtspWatchdogPollMs = 100;
    double rtspRetryMultiplier = 2.0;
    int sourcePollTimeoutMs = 200;
    double offlineSamplingToleranceMs = 1.0;
    // Explicit GStreamer defaults for the compressed branch queues.
    int gstQueueMaxBuffers = 200;
    int gstQueueMaxBytes = 10485760;
    int gstQueueMaxTimeMs = 1000;
    int analysisDropOldFrames;
    int analysisMaxBuffers;
    int encodedMaxBuffers;
    int recordEnabled;
    double recordRingSec;
    int writerQueueMaxMessages;
    int writerAppsrcMaxBytes = 200000;
    double writerEosTimeoutSec = 10.0;

    // HTTP is the shipped destination. "file" is retained for regression/replay.
    std::string recordDestination = "file";
    std::string recordServerUrlEnv = "VIDEO_SERVER_URL";
    std::string recordTokenEnv = "VIDEO_SERVER_TOKEN";
    std::string recordDeviceId = "firefly01";
    int recordRingMaxMiB = 32;
    int writerQueueMaxMiB = 32;
    int httpBufferMaxMiB = 8;
    int httpFragmentMs = 1000;
    double httpTimeoutSec = 5.0;
    // Set by main for each worker; secrets never enter runtime-settings/logs.
    std::string recordServerUrl, recordToken, recordCameraId, recordSessionId;

    // Launcher/measurement settings are exported by --runtime-settings after
    // the same strict validation used by the camera workers.
    int statusIntervalSec = 10;
    int systemSampleIntervalSec = 10;
    int affinitySnapshotAfterSec = 15;
    int launcherPollMs = 1000;
    int evidenceSampleClipCount = 2;
    int evidenceSampleMaxMiB = 20;
};

Config loadConfig(const std::string& path);
void writeRuntimeSettings(const Config& config, std::ostream& output);

struct CameraConfig {
    std::string id, urlEnv, codec;
    double gopHintSec = 0;
};

std::vector<CameraConfig> loadCameras(const std::string& path);
