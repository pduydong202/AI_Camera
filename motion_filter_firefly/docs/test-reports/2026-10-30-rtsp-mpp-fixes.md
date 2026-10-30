# Kiểm thử bản sửa RTSP/MPP — 30/10/2026

**Kết quả:** bản thử đạt kiểm tra trên Firefly và receiver Optiplex trước khi đưa vào mã chính.

Nguồn H.264: 1920×1080, 20 FPS; tám kết nối tới cùng nguồn giả lập RTSP.

| Bài thử bản cuối | Kết quả |
|---|---|
| H.264, 8 luồng × 300 giây | 168 clip; 20.00–20.01 FPS/luồng |
| H.264, 8 luồng × 180 giây, timestamp lỗi/mạng khựng | 102 clip |
| H.264, 8 luồng × 180 giây, ngắt 10 và 15 giây | 88 clip; cả 8 luồng có hình trở lại sau mỗi lần ngắt |
| H.265, 640×360/20 FPS, ngắt/nối ngắn | 2 clip, 2 phiên có hình |

- Nhận **360/360 clip**; hash, danh tính và PTS/DTS đạt. Giải mã toàn bộ **77,166 frame**, không lỗi; không lỗi HTTP hoặc cảnh báo deinit/pool MPP.
- Nguồn bình thường: bước PTS tối đa **50 ms** ở cả hai nhánh, không bỏ ảnh phân tích. Bài tạo lỗi vẫn có bỏ ảnh phân tích khi dữ liệu dồn; bộ đếm ghi nhận đúng, không sửa PTS để che khoảng trống.
- Bài nguồn bình thường: CPU trung bình **51.2% toàn máy**, nhiệt cao nhất trong các thermal zone **47.2°C**, tổng RSS worker + supervisor **1316–1332 MiB**. Bài ngắn chưa đủ kết luận độ ổn định dài hạn.

**Bản sửa:** mặc định RTSP mode 0; chờ IDR/IRAP trước tee lúc mở nguồn; chặn depayloader xả hình nhiều slice chưa hoàn chỉnh khi EOS; build/chọn plugin Rockchip đã sửa deinit trong prefix riêng. Không thay thư viện hệ thống. L1/L2/L3 và thời gian chờ reconnect giữ nguyên.

Build và toàn bộ `check.sh` đạt, gồm regression giữ nguyên byte/PTS của AU đầy đủ và loại đuôi AU bị cắt. Vòng thử trước đã phát hiện frame cuối lỗi; kết quả trên thuộc vòng chạy lại sau khi sửa guard.

**Giới hạn:** chưa chạy lại 8.000 giây với bản cuối; H.265 mới kiểm thử ngắn. Cần bài dài và camera thật trước khi kết luận vận hành lâu dài.

Chứng cứ được Git bỏ qua tại `build_deploy/rtsp_release_20261030/`: `final_release_evidence.tar.gz`, `receiver_final_audit.json`, `release_v2_results.json`.
