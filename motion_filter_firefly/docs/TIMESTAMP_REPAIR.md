# Sửa metadata MP4 phân mảnh — 27/10/2026

## Nguyên nhân tìm thấy trên thiết bị

Sau khi bỏ parser dư ở bước remux, Firefly gửi thành công 11/11 sự kiện nhưng
người dùng thấy VLC chớp trắng một frame. Kiểm tra qua SSH cho thấy các clip có
PTS trùng, có đoạn nhiều frame cùng thời điểm, dù giải mã phần mềm không có
frame trắng toàn ảnh. Chưa thể kết luận mọi chớp trắng của VLC đều do metadata.

Firefly dùng GStreamer 1.20.3. Trong fragment thực tế, khi duration của một sample
khác default ban đầu, muxer bỏ default duration ở `tfhd` và ghi duration vào các
`trun` đã có. Các `trun` tạo sau, khi duration trở lại giá trị ban đầu, lại bỏ
trường duration. Vì `trex` cũng có default 0, demuxer đọc ra duration 0, dẫn đến
PTS trùng. Nhánh tối ưu sample size có cùng dạng lỗi. Logic tương ứng nằm trong
`atom_traf_add_samples` của
[GStreamer 1.20.3 atoms.c](https://github.com/GStreamer/gstreamer/blob/1.20.3/subprojects/gst-plugins-good/gst/isomp4/atoms.c).

## Cách sửa

`event_receiver/fmp4_timing.py` khôi phục default duration/size từ sample đầu tiên
đã ghi giá trị tường minh trong chính fragment. Chỉ áp dụng khi default tương
ứng bị thiếu, default kế thừa bằng 0 và có run cần dùng default. Không nội suy
timestamp theo FPS, không bỏ frame hoặc encode lại. Module cập nhật kích thước
box và data offset tương đối; payload `mdat` được chép nguyên byte.

Chỉ sửa bố cục streaming một track, không index do sender hiện tại tạo. Khi cần
sửa mà có index, offset tuyệt đối hoặc box chưa hỗ trợ, bộ nhận từ chối thay vì
đoán vị trí dữ liệu. Giới hạn metadata 32 MiB; media được chép từng khối.

Sau đó bộ nhận remux trực tiếp `qtdemux.video_0 → mp4mux`, probe packet và chỉ
báo READY khi PTS duy nhất, DTS tăng nghiêm ngặt. `event.json` chứa thống kê
`fragment_defaults` và `timing`; log có `[TIMING REPAIR]` nếu đã khôi phục default.

Sửa này nằm trên Optiplex. Khi chép mã, luôn đặt `server.py` và `fmp4_timing.py`
cùng thư mục; `recheck_failed.py` cũng dùng các module đó. Không cần build lại
Firefly cho bước sửa metadata này.

## Bằng chứng kiểm thử

- WSL: 9 tình huống tích hợp HTTP, 12 test receiver và 3 test metadata PASS.
  Case jitter 0,5 ms tái hiện đúng nhánh lỗi; đối chiếu picture NAL nén,
  PTS/DTS/keyframe trước/sau truyền. Fixture 1080p lấy từ video test `15.mp4`.
- Optiplex: 15 test Python PASS. Hai mẫu lỗi thật `84b148cc...` (135 packet),
  `dbd806d5...` (149 packet) được sửa trên bản sao, decode hết, PTS duy nhất,
  DTS tăng nghiêm ngặt. SHA-256 payload `mdat` trước/sau giống nhau; gốc giữ nguyên.
- Bài native 180 giây: Firefly run `20261027T115856_pid35626`, session
  `20261027T045857.235568Z_pid35666`, **12 event / 12 gửi thành công / 0 lỗi gửi**.
  Phân tích và decode khoảng 20 fps; process failures 0.
- Toàn bộ 12 clip: **2.828 packet → 2.828 frame**, không lỗi decoder, không PTS
  trùng hoặc DTS không tăng. Quét luma ở kích thước 320 px không thấy frame trắng
  toàn ảnh (`YAVG >=230`); luma lớn nhất khoảng 142. Một khoảng nhảy 262 ms ở
  clip 3 khớp timestamp encoded nguồn trên Firefly (261,819 ms); giữ nguyên nó.

Báo cáo chi tiết trên Optiplex:
`~/AI_Camera_http_test_20261027/event_receiver/quality_reports/20261027T045857.235568Z_pid35666.json`.
Bản sao phát triển:
`motion_filter_firefly/build_deploy/quality_native_20261027/timing_fixed_quality_report.json`.

## Xem lại bằng VLC

Các clip mới nằm tại:
`~/AI_Camera_http_test_20261027/event_receiver/received/firefly01/cam01/20261027T045857.235568Z_pid35666/`.
Ví dụ mở `7/clip.mp4`. Clip cũ được giữ nguyên, vẫn có thể có timestamp lỗi.
Phép quét frame và full decode không quan sát được cửa sổ VLC hay lỗi render
GPU; cần người dùng xác nhận trực quan trên clip mới. Chưa xác nhận H.265,
nhiều camera, mạng khác LAN hoặc chạy dài ngày.
