# Giới hạn và vấn đề đang biết

## Plugin MPP của thiết bị thử

Máy thử dùng GStreamer 1.20.x, OpenCV 4.5.4, plugin rockchipmpp báo version 1.14.4; đường dẫn `/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrockchipmpp.so`. Khi worker dừng có hai dòng `mpp_frame_deinit` bất thường và log pool còn một MppFrame 224 byte. Lỗi này cũng đã xuất hiện với bài gst-launch decode-only, ngoài ứng dụng.

Đã đối chiếu binary thực tế (SHA-256 `d9270559339dd60b057ba8afdafdbc41ca3f6315402674475fd1f6a93f323863`) và thấy lời gọi cũ truyền `self->mpp_frame` thay vì địa chỉ `&self->mpp_frame`. API `mpp_frame_deinit` nhận `MppFrame*`. Upstream có sửa tại commit [6c90d19b502e36f6228b2d3d31230363bb690b2d](https://github.com/JeffyCN/mirrors/commit/6c90d19b502e36f6228b2d3d31230363bb690b2d) của nhánh gstreamer-rockchip.

Từ bản sửa 30/10, `build.sh` build source upstream đã pin vào prefix riêng
`build_gst/`; `run.sh` chọn và kiểm tra đúng plugin này trước khi chạy worker.
Không thay binary/thư viện hệ thống và không kèm binary plugin trong Git.
Xem [cách build và chọn plugin](ROCKCHIP_PLUGIN.md). Chạy binary trực tiếp mà
không đặt môi trường plugin vẫn có thể dùng bản hệ thống lỗi. Chỉ số version
1.14.4 không xác định được commit.

## Tải và timeline

8 kết nối tới nguồn mô phỏng đã đạt gần 20 fps trong 30 phút với cấu hình mặc định. 10 camera chưa được xác nhận. 15 kết nối không đạt; bỏ ghim CPU không cải thiện. Không có trần camera 8/10 hardcode trong ứng dụng.

Ở bài 15 kết nối/10 phút, đã thấy nhiều PTS được sửa, encoded offset tối đa khoảng 5,52 giây. Clip mẫu giải mã được và có timestamp tăng dần không chứng minh mốc cắt luôn khớp sự kiện khi hai nhánh sửa PTS khác nhau. Bài toàn CPU chỉ 3 phút không đủ để kết luận đã giải quyết vấn đề này.

Các số đo trên thuộc bản cũ. Bản sửa ngày 29/10 chuẩn hóa PTS chung trước tee,
dùng MP4 độ phân giải microgiây và tăng hàng đợi TCP receiver; các bài H.264
8 luồng nguồn thật/tạo lỗi đã qua đối chiếu clip. Xem [báo cáo kiểm thử](test-reports/2026-10-29-pts-http-validation.md).
Bài [8.000 giây ngày 29/10](test-reports/2026-10-29-8-streams-8000s-reconnect.md)
đã chạy trên bản trước. Bản sửa 30/10 có kiểm thử lại H.264/H.265, timestamp
lỗi và ngắt/nối; chưa chạy lại đủ 8.000 giây. Không suy ra mọi lỗi mạng hoặc
độ khớp pre/post-roll với thời gian chụp thật chỉ từ timestamp hợp lệ của clip.

`queue_age_ms` đo từ khi buffer decoded đến appsink tới lúc lấy ra; `local_delay_ms` đo từ mốc đó đến quyết định xong. Đây không phải tổng độ trễ camera→ứng dụng: chưa tính thời gian encode nguồn, mạng và jitterbuffer trước decoder.

Các cửa sổ thuật toán dựa trên frame phân tích. Quá tải/bỏ frame có thể đổi tính chất phát hiện. Giữ mốc 8 kết nối đã đo, đánh giá lại với camera thực, bitrate và cảnh thực tế khi tăng tải.

## Chất lượng phát hiện và ghi hình

Chớp sáng, đổi toàn cảnh lúc lặp video và cây rung có thể tạo event. Các cảnh mẫu đã kiểm tra được người dùng chấp nhận một phần; chưa có tập gán nhãn độc lập để báo cáo precision/recall hoặc khẳng định lọc được mọi nền động. L2 8 bit có giới hạn; đổi riêng sang 16 bit đã gây mất frame xe trong replay nên không đưa vào bản này.

H.265 đã có smoke test native và ngắt/nối ngắn; chưa có bài tải 8 luồng dài
như H.264. Pre-roll phụ thuộc keyframe có sẵn; warmup/reconnect có thể chưa
đủ lịch sử. Khi RTSP mất giữa một hình nhiều slice, guard EOS bỏ phần đuôi
chưa chắc hoàn chỉnh thay vì ghi hình lỗi vào clip. Không khôi phục được
payload đã mất từ nguồn. Không encode lại clip gốc.

Đường HTTP mới gửi trực tiếp video tới Optiplex, giới hạn ring/queue theo byte;
không có replay từ đĩa Firefly khi mạng mất lâu. Chưa có retention/xóa clip đã
nhận tự động. Video mask là chẩn đoán phụ của file mode; HTTP mode yêu cầu tắt
validation/L2 capture. Xem [NETWORK_DELIVERY.md](NETWORK_DELIVERY.md).
