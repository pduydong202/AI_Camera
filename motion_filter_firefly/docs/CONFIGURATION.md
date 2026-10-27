# Cấu hình

`config.yaml` là cấu hình vận hành duy nhất; danh sách camera nằm cuối file. Dùng cú pháp OpenCV YAML (`%YAML:1.0`), cờ số nguyên 0/1. Mọi khóa đều bắt buộc. Loader từ chối khóa sai/trùng, sai kiểu, NaN, giá trị ngoài miền cho phép và ring ngắn hơn pre-roll. Sửa YAML rồi khởi động lại; chưa có hot reload.

## Đầu ra HTTP mới

Mặc định `record_destination: "http"`; cấu hình cũ cần bổ sung các khóa bên dưới.
URL/token lấy từ `.env` qua launcher; chạy binary trực tiếp thì export các biến
tương ứng. Hướng dẫn đầy đủ: [NETWORK_DELIVERY.md](NETWORK_DELIVERY.md).

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `record_destination` | `http` | `http` gửi thẳng, `file` giữ recorder local để đối chiếu |
| `record_server_url_env` | `VIDEO_SERVER_URL` | Tên biến chứa base URL LAN/VPN/HTTPS |
| `record_token_env` | `VIDEO_SERVER_TOKEN` | Tên biến chứa bearer token, không log giá trị |
| `record_device_id` | `firefly01` | ID riêng cho từng thiết bị gửi |
| `record_ring_max_mib` | 32 | Trần byte ring packet nén, áp dụng cả file/HTTP |
| `writer_queue_max_mib` | 32 | Trần byte packet chờ writer HTTP |
| `http_buffer_max_mib` | 8 | Trần riêng cho appsrc và bộ đệm upload HTTP |
| `http_fragment_ms` | 1000 | Khoảng fragment MP4; 100..10000 ms |
| `http_timeout_sec` | 5 | Timeout mạng/hoàn tất; 1..60 giây, dài hơn fragment |

Các ngân sách RAM phải trong 1..1024 MiB. Tổng chờ EOS + HTTP + khoảng dự phòng
phải nhỏ hơn deadline shutdown watchdog. HTTP mode yêu cầu tắt validation và
L2 capture để không ghi dữ liệu video chẩn đoán trên Firefly. Hết byte/message
budget làm rơi sự kiện thay vì chặn luồng phân tích; xem `SEND FAILED` trong log.

Giá trị mặc định của bản giao giữ hành vi đã đo. Các giá trị khởi tạo trong `Config` phục vụ đối tượng C++/test; loader không dùng chúng để âm thầm bù khóa YAML bị thiếu. Các YAML từ gói thử nghiệm cũ không phải cấu hình tương thích để chép đè.

## Ảnh, tốc độ và CPU

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `output_root` | `output` | Thư mục đầu ra; đường tương đối tính từ thư mục dự án khi dùng run.sh |
| `analysis_max_width` | 854 | Chiều rộng phân tích tối đa; tối thiểu 2 |
| `analysis_max_height` | 480 | Chiều cao phân tích tối đa; tối thiểu 2 |
| `analysis_allow_upscale` | 0 | Cho phép phóng nguồn nhỏ; kích thước NV12 được làm chẵn |
| `analysis_target_fps` | 20.0 | FPS mục tiêu, >0; không thể tạo thêm frame cho nguồn chậm |
| `analysis_sampling_enabled` | 1 | Giới hạn nhịp phân tích live/media; 0 chỉ dùng đo có kiểm soát |
| `analysis_rate_burst_ms` | 200 | Credit bổ sung cho bắt kịp ngắn hạn ở RTSP; không tăng queue |
| `analysis_sparse_l3_enabled` | 1 | Tính L3 trong vùng foreground; tối ưu đã kiểm thử tương đương |
| `opencv_threads` | 1 | Số thread OpenCV mỗi worker |
| `main_cpu_list` | `4-7` | Ghim riêng main thread sau khi nguồn có frame; rỗng = không ghim. Các thread đã tạo giữ mask cũ |
| `fallback_fps` | 30.0 | Chỉ dùng khi caps/duration không cung cấp tốc độ hợp lệ |
| `startup_warmup_sec` | 2.0 | Warmup theo PTS mỗi session mới; không mở sự kiện trong warmup |

Nếu nguồn 1920×1080, kích thước thực tế mặc định là 854×480. Đối với tỷ lệ nguồn khác, kích thước phân tích được tính lại; không ép méo ảnh. FPS thực tế khi quá tải có thể thấp hơn target.

## Thuật toán và sự kiện

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `l1_history_frames` | 500 | History của MOG2, số nguyên >0 |
| `l1_var_threshold` | 16.0 | Ngưỡng phương sai của MOG2, >0 |
| `l1_learning_rate` | -1.0 | -1: OpenCV tự học; [0,1]: learning rate cố định |
| `l1_open_kernel_size` | 3 | Cạnh kernel vuông OPEN; nguyên dương lẻ. **3 giữ phép OPEN 3×3**; 1 làm phép này trở thành đồng nhất |
| `l2_history_frames` | 0 | 0 theo history MOG2; >0 là cửa sổ L2 độc lập |
| `l2_repeat_neighborhood_size` | 3 | Cạnh neighborhood vùng lặp, đơn vị ô lưới; nguyên dương lẻ |
| `l3_location_neighborhood_size` | 3 | Cạnh neighborhood kế thừa tuổi/bảo vệ vị trí, đơn vị pixel; nguyên dương lẻ |
| `component_connectivity` | 8 | Liên thông thành phần, 4 hoặc 8 |
| `tawl_temporal_fps_ratio` | 0.20 | TA=ceil(ratio×fps danh nghĩa), ≥1; phải nằm trong bộ đếm tuổi 16 bit |
| `tawl_width_frame_ratio` | 0.05 | W=ceil(ratio×chiều rộng phân tích), ≥1; ratio trong (0,1] |
| `event_min_valid_ratio` | 0.005 | Ngưỡng VALID trên tổng diện tích ảnh; trong (0,1] |
| `event_pre_sec` | 2.0 | Mục tiêu pre-roll trước sự kiện; cần keyframe trong ring |
| `event_post_sec` | 2.0 | Khoảng yên lặng trước khi đóng sự kiện, theo PTS |

Không có ngưỡng loại thành phần nhỏ theo diện tích riêng ngoài phép OPEN và ngưỡng tổng VALID. L2 giữ lượng tử hóa Otsu 8 bit, shadow detection tắt theo hợp đồng mask nhị phân. Số 0/255, kiểu dữ liệu, layout NV12, căn stride và đơn vị ns là hằng số biểu diễn, không đưa thành tham số chỉnh camera.

## Nguồn và bộ đệm

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `decoder_element` | `mppvideodec` | Element giải mã Rockchip; phải xuất NV12 phù hợp RGA |
| `rtsp_latency_ms` | 200 | Latency của jitterbuffer RTSP |
| `rtsp_buffer_mode` | 3 | Enum GStreamer rtspsrc: 0 none, 1 slave, 2 buffer, 3 auto, 4 synced |
| `rtsp_drop_on_latency` | 0 | Cờ drop-on-latency của rtspsrc |
| `rtsp_use_tcp` | 1 | 1 ép RTP interleaved TCP; 0 cho rtspsrc chọn transport |
| `source_poll_timeout_ms` | 200 | Thời gian chờ mỗi lượt đọc appsink/bus; cũng ảnh hưởng nhịp quan sát dừng |
| `offline_sampling_tolerance_ms` | 1.0 | Dung sai lịch lấy mẫu file, không áp dụng token budget live; ≥0 và ngắn hơn một frame target |
| `gst_queue_max_buffers` | 200 | Giới hạn buffer của queue nén trước decoder/encoded appsink; default GStreamer trước đây |
| `gst_queue_max_bytes` | 10485760 | Giới hạn byte của các queue trên; default trước đây |
| `gst_queue_max_time_ms` | 1000 | Giới hạn thời gian media của các queue trên; default trước đây |
| `analysis_drop_old_frames` | 1 | Appsink decoded bỏ ảnh cũ khi đầy để hạn chế độ trễ |
| `analysis_max_buffers` | 8 | Sức chứa appsink decoded; cũng là credit nền của bộ giới hạn live |
| `encoded_max_buffers` | 512 | Sức chứa appsink nhánh nén, không drop |

Mỗi `queue` GStreamer chặn khi chạm một trong ba giới hạn. Giá trị 0 tắt giới hạn tương ứng, nhưng không được tắt cả ba. Đây là hai queue **trước** appsink, khác với `analysis_max_buffers`/`encoded_max_buffers`. Tăng hàng đợi không làm nhanh xử lý; khi đầu ra chậm kéo dài sẽ tăng dữ liệu chờ và/hoặc backpressure.

## Ghi clip và chẩn đoán

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `record_enabled` | 1 | Bật nhánh nén và recorder clip gốc |
| `record_ring_sec` | 4.0 | Ring nén tối thiểu; thời gian giữ còn tính đến GOP/pre-roll |
| `record_safety_sec` | 1.0 | Khoảng dự phòng thêm cho ring nén |
| `writer_queue_max_messages` | 1024 | Queue hữu hạn đưa message vào writer, chặn khi đầy |
| `writer_appsrc_max_bytes` | 200000 | Giới hạn appsrc của pipeline MP4; default GStreamer trước đây, block=true |
| `writer_eos_timeout_sec` | 10.0 | Chờ muxer EOS; hết hạn báo lỗi, không công bố clip hoàn chỉnh |
| `validation_enabled` | 0 | Ghi video mask L1/L2/L3/VALID quanh sự kiện; tốn tài nguyên phụ |
| `validation_ring_sec` | 4.0 | Ring ảnh mask, ≥pre-roll |
| `validation_queue_max_messages` | 512 | Queue writer mask |
| `validation_fourcc` | `mp4v` | FourCC 4 ký tự cho video mask, không dùng encode clip gốc |
| `frame_stats_enabled` | 0 | Ghi CSV từng frame phân tích |
| `stats_interval_sec` | 1.0 | Chu kỳ log; PERF dùng wall clock, log frame dùng PTS |
| `perf_stats_enabled` | 1 | Counter decode, latency, thời gian wall/thread CPU và chẩn đoán PTS |
| `l2_capture_enabled` | 0 | Capture liên tục mask/density/PTS lossless cho phân tích offline |

Ring nén và ring mask phải đủ pre-roll. Bật video mask/capture làm đổi tải đo. Video mask có FPS danh nghĩa, không thay thế timeline chính xác của clip nén. Lỗi writer clip gốc được truyền về ứng dụng; đầu ra mask chỉ là chẩn đoán phụ.

## Watchdog và reconnect

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `rtsp_reconnect_enabled` | 1 | Supervisor tự hồi phục cho RTSP; không áp dụng file offline |
| `rtsp_startup_timeout_sec` | 20.0 | Hạn chờ frame đầu tiên |
| `rtsp_stall_timeout_sec` | 15.0 | Hạn không có tiến triển frame |
| `rtsp_shutdown_timeout_sec` | 20.0 | Hạn cho worker đóng clip/nguồn trước khi buộc kill |
| `rtsp_retry_initial_sec` | 1.0 | Backoff ban đầu |
| `rtsp_retry_max_sec` | 30.0 | Trần backoff; ≥initial |
| `rtsp_retry_reset_sec` | 30.0 | Đủ thời gian có frame để reset backoff |
| `rtsp_watchdog_poll_ms` | 100 | Nhịp kiểm tra supervisor; >0 và không dài hơn các deadline |
| `rtsp_retry_multiplier` | 2.0 | Hệ số tăng backoff, ≥1 |

Các thời gian watchdog phải trong (0,86400] giây. Source poll và writer EOS wait phải ngắn hơn deadline liên quan khi bật reconnect. Shutdown deadline là giới hạn cuối cho **toàn bộ** cleanup, kể cả khi vendor call hoặc queue không trả về. Tiến trình bị buộc kill có thể giữ lại file `.part`.

## Launcher và gói chứng cứ

| Khóa | Mặc định | Ý nghĩa |
| --- | --- | --- |
| `status_interval_sec` | 10 | Khoảng giữa các dòng trạng thái launcher |
| `system_sample_interval_sec` | 10 | Khoảng giữa các lần lấy counter CPU, nhiệt, RSS |
| `affinity_snapshot_after_sec` | 15 | Chờ trước snapshot affinity; 0 lấy ở lượt đầu |
| `launcher_poll_ms` | 1000 | Nhịp kiểm tra tiến trình/thời hạn; không dài hơn status/sample interval |
| `evidence_sample_clip_count` | 2 | Số clip mẫu tối đa từ camera đầu; rải đều, mặc định đầu/cuối; 0 không lấy mẫu |
| `evidence_sample_max_mib` | 20 | Tổng dung lượng clip mẫu tối đa theo MiB; 0 không lấy mẫu |

Nhịp là khoảng kiểm tra, không phải đồng hồ thời gian thực: công việc gom số liệu có thể làm dòng `elapsed` lệch nhẹ. Thời lượng chạy và số replica là **lựa chọn của từng lệnh** qua `--seconds`/`--replicas`, không phải ngưỡng phát hiện hardcode. Giá trị các nhịp/budget được binary kiểm tra rồi xuất `runtime_settings.tsv`; Bash không tự có bộ default khác.

Giới hạn MiB chỉ tính byte video mẫu, không giới hạn toàn bộ log/chứng cứ. FFmpeg/ffprobe thiếu hoặc không được gọi thì không được coi là đã kiểm tra decode. Full video không bị xóa khi không được chọn làm mẫu.

## Danh sách camera và biến môi trường

Mỗi phần tử `cameras` bắt buộc có đúng bốn khóa:

| Khóa | Ví dụ | Ý nghĩa |
| --- | --- | --- |
| `id` | `cam01` | ID duy nhất, chỉ chữ/số/`_`/`-`; dùng đặt thư mục/log |
| `url_env` | `CAM01_RTSP` | Tên khóa trong .env; không đặt URL/mật khẩu trong YAML |
| `codec` | `h264` | `h264` hoặc `h265`; phải khớp nguồn |
| `gop_hint_sec` | 0.0 | ≥0; 0 học GOP quan sát được, >0 bổ sung gợi ý giữ ring |

`MOTION_CONFIG_FILE`, `MOTION_ENV_FILE` có thể chọn file cấu hình/env khác khi chạy launcher. `MOTION_FILTER_BIN` là override cho kiểm thử/tích hợp. `MOTION_RUN_ID`, `MOTION_LAUNCHER_PID` do launcher truyền xuống để gắn đầu ra và dọn tiến trình; không khai báo chúng trong `.env`. `.env` chỉ được dùng để tra URL theo danh sách YAML.
