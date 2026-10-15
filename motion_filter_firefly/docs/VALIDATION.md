# Chứng cứ và phạm vi xác nhận

Ngày tổng hợp: 15/10/2026. Phân biệt kết quả chạy trên Firefly với kiểm thử mã của bản gom cấu hình. Không dùng số replica thay cho số camera có cảnh/bitrate độc lập.

## Chạy thực tế trên Firefly RK3588

| Run ID chứng cứ | Phép thử | Kết quả chính |
| --- | --- | --- |
| `20261015T140348_pid62889` | 8 kết nối ×30 phút, main 4–7 | Sau warmup 19,9976–20,0010 fps/kết nối; CPU toàn máy 52,16%; không reconnect/forced kill/app error |
| `20261015T150006_pid81623` | 15 kết nối ×10 phút, main 4–7 | 14,2345–14,9192 fps; CPU 80,87%; các lõi lớn gần bão hòa; không đạt 20 fps |
| `20261015T153607_pid93360` | 15 kết nối ×3 phút, main không ghim | 13,77–14,67 fps; CPU 99,19%, mọi lõi khoảng 99%; không cải thiện |

Nguồn H.264 1920×1080 phát 20 fps từ Windows/MediaMTX. Kích thước phân tích 854×480, OpenCV 1 thread/worker. Bài 8 và 15 ghim main giữ các worker thread đã tạo ở mask 0–7 theo snapshot. Các run có thời lượng/cảnh ở thời điểm khác nhau; không suy ra khả năng tải chính xác cho mọi camera hoặc hành vi nhiệt/bộ nhớ lâu dài.

Bài 8 kết nối có queue age trung bình khoảng 21 ms và độ trễ cục bộ decoded→quyết định khoảng 55 ms; mức lớn nhất quan sát 337,51 ms. Không thấy độ trễ tăng liên tục trong run đó. Nhiệt độ cao nhất 61°C; cuối bài còn xu hướng tăng nên chưa chứng minh cân bằng nhiệt dài hạn. Có một số frame decoded không được phân tích, không phải mọi frame nguồn đều được giữ.

Bài 15 ghim main: queue age khoảng 340 ms, độ trễ cục bộ khoảng 407 ms, tối đa 805,48 ms. Nhiều PTS phải sửa; encoded offset cực đại 5.520,356 ms, analysis cực đại 1.964,088 ms. Không kết luận mốc cắt sự kiện chính xác dưới tải này chỉ từ khả năng giải mã clip.

Bài 15 không ghim: decoder vẫn khoảng 20 fps nhưng phân tích chỉ khoảng 14 fps; queue age 344–350 ms, độ trễ cục bộ 414–421 ms, tối đa 839,12 ms. RSS đã thu đúng 15/15 worker; mẫu cuối khoảng 107.780–108.452 KiB/worker. Bài chỉ 3 phút, không đủ xác nhận không rò bộ nhớ. Một camera vẫn có sửa PTS; thời lượng ngắn hơn không chứng minh giải pháp toàn CPU đã sửa timeline.

### Clip và đối chiếu nguồn

- Run 8 kết nối: kiểm tra metadata 899 clip, không `.part`, hai clip mẫu tổng 383 frame được giải mã đầy đủ, timestamp tăng dần và mở đầu bằng keyframe. Hash 23 source/header khớp mã dùng trong phép thử.
- Run 15 ghim: 484 clip qua metadata; hai clip mẫu tổng 774 frame được giải mã đầy đủ; không `.part` hoặc lỗi ứng dụng. Hash source/script đã đối chiếu.
- Run 15 không ghim: 141 clip, không `.part`, không media check failure; cả hai mẫu có decode exit=0 theo gói thu trên Firefly; mọi supervisor exit=0, attempts=1, không forced kill. Hash 23 source/header và 4 script khớp mã trước khi gom cấu hình. RSS worker đã được thu theo PID/PPID.

Các clip còn lại chỉ được probe metadata trong các gói này; không gọi đó là đã xem hoặc decode toàn bộ. Cảnh báo MPP khi dừng còn xuất hiện ở mọi run.

### Recovery và chất lượng thuật toán

Phép thử mất publisher RTSP trong khoảng 30 giây với MediaMTX còn chạy đã phục hồi: hai session có frame và qua warmup, forced kill=0, app error=0, `.part`=0. Sau đó publisher được kết nối lại theo backoff; log có các lần thử lỗi trong thời gian nguồn vắng. Đây là kiểm chứng native cho cơ chế recovery trước khi gom tham số.

Video mẫu xe, mưa, cây rung đã được thu để đối chiếu sự kiện; người dùng chấp nhận một số event do chớp sáng/đổi cảnh khi lặp. Không có số đo precision/recall tổng quát. Phép diagnostic L2 cho thấy 16-bit-only có thể triệt mất frame xe xác nhận, nên bị loại. Capture liên tục L2 3.568 frame đã qua CRC, chỉ số liên tiếp, mật độ/mask count native khớp. Giữ L2 8 bit và logic hiện tại.

## Kiểm thử bản gom mã

Chạy trên Linux x86_64 với OpenCV 4.6, GStreamer 1.24 và header RGA chính thức. Firefly thực tế dùng OpenCV 4.5.4/GStreamer 1.20.x; máy phát triển không có phần cứng MPP/RGA để chạy đường decode/resize thật.

| Nhóm kiểm thử | Kết quả |
| --- | --- |
| config | PASS: cấu hình/camera hợp lệ; 33 trường hợp sai bị từ chối; URI credentials được che |
| affinity | PASS: cú pháp CPU list và chỉ ghim main, giữ mask thread đã tạo |
| recovery | PASS: 10 tình huống healthy, transient, treo khởi động/frame/cleanup, crash/fatal và hủy; không worker chồng/orphan |
| recorder | PASS: 80 access unit, compressed picture/PTS/DTS giữ nguyên; hoàn tất rename, truyền lỗi writer; clip giải mã hết |
| sparse_l3 | PASS: 1.300 frame, mọi trường FrameResult, bốn mask và tín hiệu event bằng tham chiếu; ROI 10,2325%, empty-fast 768 |
| algorithm_config | PASS: kernel 1/3 thay đổi đúng phép OPEN, history L2 theo MOG hoặc độc lập; 180 frame parity sparse/toàn ảnh với neighborhood 5 và connectivity 4 |
| l2_capture | PASS: mask lossless, padded/odd stride, bitpack/RLE, float density, PTS64; từ chối CRC lỗi/footer thiếu |
| live_budget | PASS: budget live và thống kê latency |
| pts_diagnostics | PASS: nguyên nhân raw, chuỗi sửa 1.000 frame, merge window, tách nhánh; không đổi quy tắc sửa cũ |
| stage_timing | PASS: wall/thread CPU độc lập và reset mỗi frame |
| log_line | PASS: 8.000 record từ 8 thread không mất/chen trường |
| launcher | PASS: replica, URL literal, dry-run không lộ credential, nhịp YAML, dừng hữu hạn/hủy, cleanup; từ chối chạy chồng/thiếu URL/fatal child |

Bộ gom chứng cứ cũng đã qua phép thử clip MP4 thật: số mẫu 0/2/3, tắt lấy mẫu bằng budget 0, trần byte bỏ clip vượt dung lượng, giữ lựa chọn đầu/cuối mặc định và ghép RSS theo PID worker. Đã đối chiếu các default queue/appsrc bằng GObject của GStreamer.

Đã kiểm tra cú pháp tất cả translation unit production với header OpenCV/GStreamer/RGA thật và `-Wall -Wextra -Wpedantic -Werror`. Bộ tham chiếu thuật toán được giữ nguyên byte, không sửa để làm test mới qua. Bộ self-test không kết nối nguồn RTSP thực tế; dùng fixture tổng hợp và tiến trình giả lập vòng đời.

Đây là bản tái cấu trúc tham số với default giữ nguyên: MOG2 500/16/-1, kernel/neighborhood 3, liên thông 8, L2 theo history MOG2, các queue GStreamer/appsrc và thời gian chờ giữ giá trị trước đó. Điều này cộng với parity test hỗ trợ tính tương đương mặc định, nhưng **không thay cho build/chạy trên thiết bị**. Lệnh xác nhận ngắn: `bash build.sh`, `bash check.sh`, rồi `bash run.sh --replicas 8 --seconds 180`. Không yêu cầu thêm bài 15 kết nối để chốt gói này.

Chốt: mốc đã kiểm chứng là 8 kết nối gần 20 fps với nguồn thử. Chưa xác nhận 10 camera, H.265 native, chạy dài ngày, bộ nhớ/driver hoàn toàn sạch hoặc chất lượng phát hiện với mọi cảnh.
