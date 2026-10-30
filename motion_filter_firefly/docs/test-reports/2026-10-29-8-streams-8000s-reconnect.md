# Kết quả 8 luồng / 8.000 giây, có ngắt camera — 29/10/2026

Chạy bản thử đã sửa PTS và HTTP trên **cả Firefly và receiver Optiplex**, từ **10:35:05 đến 12:48:25 (UTC+7)**. Tám kết nối tới cùng nguồn giả lập H.264 1080p/20 FPS, phân tích 854×480; người vận hành chủ động ngắt nguồn khoảng một phút lúc 12:14. Không thay thuật toán L1/L2/L3.

| Hạng mục | Kết quả |
|---|---|
| FPS phân tích khi có nguồn, sau warmup | **19,9998 FPS/luồng**; không giảm về cuối |
| CPU toàn Firefly | Trung bình **50,98%**, cao nhất **56,66%** theo mẫu khoảng 10 giây |
| Nhiệt độ Firefly | Cao nhất **57,31°C**; không thấy giảm FPS kéo dài khi nhiệt tăng |
| Bộ nhớ worker + supervisor | Tổng RSS cao nhất **1,31 GiB**; trung bình cuối bài **1,30 GiB** |
| Độ trễ xử lý cục bộ | Trung bình **50,81 ms**, cao nhất **452,39 ms** |
| Gửi/nhận sự kiện | **3.919/3.919 thành công**, không HTTP 4xx/5xx, không thiếu clip so với ACK |
| CPU toàn Optiplex | Trung bình **22,80%**, mẫu một giây cao nhất 100%; ListenOverflows/ListenDrops không tăng |

CPU chuẩn hóa toàn bộ lõi về 0–100%; RSS có thể tính lặp trang dùng chung. FPS trên loại thời gian mất nguồn và warmup; số frame sau warmup chia toàn bộ 8.000 giây tương đương **19,82 FPS/luồng**. Độ trễ cục bộ chưa phải camera → receiver. Không thu nhiệt độ Optiplex liên tục trong bài này.

## Phục hồi sau ngắt nguồn

Cả **8/8 luồng tự phục hồi**, không bị buộc dừng bằng SIGKILL. Mỗi luồng có một phiên trước ngắt, năm lần mở nguồn thất bại trong khoảng ngắt và một phiên khôi phục. Lịch chờ reconnect là **1 → 2 → 4 → 8 → 16 → 30 giây**.

Log MediaMTX do người vận hành cung cấp ghi tám phiên đọc bị đóng lúc **12:14:14**, nguồn `cam01` sẵn sàng lại lúc **12:15:04**: gián đoạn khoảng **50 giây**. Kết nối Firefly đầu tiên sau đó được MediaMTX ghi lúc **12:15:19**, khoảng **15 giây sau khi nguồn có lại**, phù hợp với lịch chờ reconnect đang ở mức 30 giây.

Clip cuối phiên cũ được nhận xong lúc **12:14:14–16**; phiên khôi phục bắt đầu **12:15:18–20**; clip đầu phiên mới được nhận xong **12:15:25–32**. Mốc nhận clip gồm cả thời gian ghi/gửi. Event ID bắt đầu lại từ 1 theo session mới là hành vi hiện tại; trong công cụ Windows chọn **Tất cả các phiên** để xem cả trước và sau ngắt.

## Kiểm tra clip và nhận xét

Đối chiếu toàn bộ **3.919 clip / 973.326 packet**: hash và danh tính khớp, không thiếu/thừa/nhận dở, không PTS trùng hoặc DTS không tăng. Giải mã **64 clip mẫu / 22.564 frame** với timebase đầu vào được giữ nguyên: **không lỗi**. Vẫn có **149 clip có khoảng PTS trên 100 ms**, lớn nhất **0,960 giây**; log nhánh nén đầu vào cũng ghi nhận gap gần 0,96 giây; chưa đối chiếu từng gap với nguồn RTSP gốc. Có 86 lần sửa timestamp ở bước chung trước chia nhánh, không cần sửa tiếp tại hai consumer. Còn cảnh báo giải phóng buffer MPP lúc đóng/reconnect; bài này chưa ghi nhận thiếu clip hoặc bộ nhớ tăng mất kiểm soát.

**Bản sửa đạt bài 8 luồng có ngắt nguồn; chưa cần thay thuật toán hoặc thiết kế lại kiến trúc cho tải này.** Nếu cần phục hồi nhanh hơn, ưu tiên xem lại giới hạn chờ reconnect và bổ sung timestamp cho log watchdog. Trước khi tăng lên 10 luồng, cần đo riêng: bốn lõi lớn đã dùng trung bình **73–74%**, còn MOG chiếm khoảng **16,2/26,5 ms** xử lý thuật toán mỗi frame. CPU tổng 51% không đồng nghĩa toàn bộ phần dư đều dùng được cho các luồng phân tích đang ghim vào lõi lớn.

Bài này chưa đại diện tám camera độc lập hoặc vận hành 24/7. Log, manifest, kết quả kiểm tra và mẫu lệnh được giữ cục bộ trong `motion_filter_firefly/build_deploy/pts_trial_20261029/soak_review_20261029/` 
