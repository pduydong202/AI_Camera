# Giới hạn và vấn đề đang biết

## Plugin MPP của thiết bị thử

Máy thử dùng GStreamer 1.20.x, OpenCV 4.5.4, plugin rockchipmpp báo version 1.14.4; đường dẫn `/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrockchipmpp.so`. Khi worker dừng có hai dòng `mpp_frame_deinit` bất thường và log pool còn một MppFrame 224 byte. Lỗi này cũng đã xuất hiện với bài gst-launch decode-only, ngoài ứng dụng.

Đã đối chiếu binary thực tế (SHA-256 `d9270559339dd60b057ba8afdafdbc41ca3f6315402674475fd1f6a93f323863`) và thấy lời gọi cũ truyền `self->mpp_frame` thay vì địa chỉ `&self->mpp_frame`. API `mpp_frame_deinit` nhận `MppFrame*`. Upstream có sửa tại commit [6c90d19b502e36f6228b2d3d31230363bb690b2d](https://github.com/JeffyCN/mirrors/commit/6c90d19b502e36f6228b2d3d31230363bb690b2d) của nhánh gstreamer-rockchip.

Gói `motion_filter` không sửa binary, không thay thư viện hệ thống, không kèm plugin đã build. Cần bản sửa tương thích từ nhà cung cấp hoặc build riêng rồi xác nhận start/stop, reconnect, clip, FPS và bộ nhớ trên thiết bị. Chỉ số version 1.14.4 không xác định được commit. Watchdog bảo vệ vòng đời worker, không sửa được lỗi quản lý tài nguyên trong plugin.

## Tải và timeline

8 kết nối tới nguồn mô phỏng đã đạt gần 20 fps trong 30 phút với cấu hình mặc định. 10 camera chưa được xác nhận. 15 kết nối không đạt; bỏ ghim CPU không cải thiện. Không có trần camera 8/10 hardcode trong ứng dụng.

Ở bài 15 kết nối/10 phút, đã thấy nhiều PTS được sửa, encoded offset tối đa khoảng 5,52 giây. Clip mẫu giải mã được và có timestamp tăng dần không chứng minh mốc cắt luôn khớp sự kiện khi hai nhánh sửa PTS khác nhau. Bài toàn CPU chỉ 3 phút không đủ để kết luận đã giải quyết vấn đề này.

Các số đo trên thuộc bản cũ. Bản sửa ngày 29/10 chuẩn hóa PTS chung trước tee,
dùng MP4 độ phân giải microgiây và tăng hàng đợi TCP receiver; các bài H.264
8 luồng nguồn thật/tạo lỗi đã qua đối chiếu clip. Xem [báo cáo kiểm thử](test-reports/2026-10-29-pts-http-validation.md).
Vẫn cần chạy lại bài 8.000 giây; không suy ra H.265, mọi lỗi mạng hoặc độ khớp
pre/post-roll với thời gian chụp thật chỉ từ timestamp hợp lệ của clip.

`queue_age_ms` đo từ khi buffer decoded đến appsink tới lúc lấy ra; `local_delay_ms` đo từ mốc đó đến quyết định xong. Đây không phải tổng độ trễ camera→ứng dụng: chưa tính thời gian encode nguồn, mạng và jitterbuffer trước decoder.

Các cửa sổ thuật toán dựa trên frame phân tích. Quá tải/bỏ frame có thể đổi tính chất phát hiện. Giữ mốc 8 kết nối đã đo, đánh giá lại với camera thực, bitrate và cảnh thực tế khi tăng tải.

## Chất lượng phát hiện và ghi hình

Chớp sáng, đổi toàn cảnh lúc lặp video và cây rung có thể tạo event. Các cảnh mẫu đã kiểm tra được người dùng chấp nhận một phần; chưa có tập gán nhãn độc lập để báo cáo precision/recall hoặc khẳng định lọc được mọi nền động. L2 8 bit có giới hạn; đổi riêng sang 16 bit đã gây mất frame xe trong replay nên không đưa vào bản này.

H.265 có đường xử lý trong mã nhưng chứng cứ native được tổng hợp ở đây dùng H.264. Pre-roll phụ thuộc keyframe có sẵn; warmup/reconnect có thể chưa đủ lịch sử. Không encode lại clip gốc.

Đường HTTP mới gửi trực tiếp video tới Optiplex, giới hạn ring/queue theo byte;
không có replay từ đĩa Firefly khi mạng mất lâu. Chưa có retention/xóa clip đã
nhận tự động. Video mask là chẩn đoán phụ của file mode; HTTP mode yêu cầu tắt
validation/L2 capture. Xem [NETWORK_DELIVERY.md](NETWORK_DELIVERY.md).
