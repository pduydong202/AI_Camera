# Xác nhận đường gửi HTTP — 27/10/2026

Đã kiểm thử phát triển trên WSL Ubuntu 22.04 và chạy native Firefly → Optiplex
Ubuntu 24.04. Sau sửa remux và metadata fragment, bài native 180 giây đạt
**12 EVENT START / 12 SEND READY / 0 SEND FAILED**, giải mã đủ **2.828 frame**,
không PTS trùng, DTS tăng nghiêm ngặt. Chi tiết và giới hạn phép đo:
[TIMESTAMP_REPAIR.md](TIMESTAMP_REPAIR.md).

## Kiểm thử phát triển ban đầu

`bash check.sh`: **PASS toàn bộ 13 nhóm**:
config, affinity, recovery, recorder, http_sender, sparse_l3, algorithm_config,
l2_capture, live_budget, pts_diagnostics, stage_timing, log_line, launcher.

Trong đó:

- Cấu hình: 43 trường hợp sai bị từ chối, bao gồm các khóa/budget HTTP mới.
- HTTP: 6 tình huống — gửi thành công, pre/post-roll thực qua EventManager/ring,
  sai token, lỗi 503 rồi phục hồi ở sự kiện sau, vượt byte budget và server treo.
- Mỗi clip nhận thành công được FFmpeg decode toàn bộ. C++/GStreamer đối chiếu
  picture NAL nén, PTS/DTS và keyframe: 80 packet cho clip đầy đủ; 91 packet cho
  clip sự kiện có pre/post-roll. Không tạo thư mục video local của sender.
- Bộ nhận: 7 test gồm health, body fixed/chunked, duplicate, auth/path/video sai,
  giới hạn byte, upload gián đoạn và MP4 bị cắt. Upload lỗi không được công bố.
- Recorder file cũ: 80 packet, hoàn tất rename, truyền lỗi writer, giải mã hết.
- Thuật toán sparse L3: 1.300 frame, mọi FrameResult/mask/event bằng tham chiếu.
- Thuật toán với tham số khác mặc định: 180 frame parity sparse/toàn ảnh.
- Watchdog: 10 tình huống vòng đời và khôi phục; launcher/cleanup đều đạt.

Các translation unit production đã sửa (`main.cpp`, `config.cpp`, `recorder.cpp`,
`http_upload.cpp`) qua `-fsyntax-only -Wall -Wextra -Wpedantic -Werror` với header
OpenCV/GStreamer/libcurl thật. Test recorder/HTTP được biên dịch, link và chạy.
Chưa full build/link binary MPP/RGA hoặc chạy decode/resize phần cứng trong WSL.

Quan sát tương thích: MP4 phân mảnh từ GStreamer trong môi trường này không được
FFmpeg cũ giải mã đúng trực tiếp. Bộ nhận remux bằng GStreamer sang MP4 thường,
không encode lại; các clip sau bước đó qua decode và đối chiếu packet nêu trên.

Phạm vi chưa xác nhận: Wi-Fi khác mạng/VPN/HTTPS,
H.265 native, nguồn có đặc tính timestamp/GOP khác fixture, tải nhiều camera và
chạy dài ngày. Các số đo tải recorder file ngày 15/10 không đại diện đường HTTP.
Làm theo [NETWORK_DELIVERY.md](NETWORK_DELIVERY.md) để xác nhận trên thiết bị.

## Cập nhật sau bài native đầu tiên

Native Firefly → Optiplex đã phát hiện 13 sự kiện, xác nhận nhận 2; 11 clip bị
receiver từ chối 422 ở remux. Log chẩn đoán tiếp theo xác nhận `Buffer has no PTS`
ở `mp4mux`. Các số PASS phát triển ở trên không chứng minh native đã hết lỗi.

Bộ nhận đã được đổi từ `qtdemux → h264parse/h265parse → mp4mux` sang remux video
trực tiếp từ `qtdemux` vào `mp4mux`. Không sinh PTS theo FPS và không bỏ buffer
thiếu timestamp. Đường này qua đối chiếu 1080p 80/91 packet trên WSL. Có công cụ
`event_receiver/recheck_failed.py` để xử lý bản sao mẫu lỗi, full decode, kiểm tra
PTS/DTS và giữ nguyên SHA-256 mẫu gốc.

Bộ kiểm tra HTTP cập nhật với đoạn 1080p từ `15.mp4`: PASS 8 tình huống tích hợp
(gửi, event, auth, 422, phục hồi 503, budget, stall, recheck) và 11 test receiver.
Lệnh đã chạy: `bash tools/check_http_sender.sh --fixture <80-frame-1080p.mp4>`.

Người dùng đã chạy recheck trên Optiplex Ubuntu 24.04 với **hai mẫu lỗi thật**:

| Mẫu | Kết quả | Packet |
| --- | --- | --- |
| `84b148cc41b640bbbd8652ef57a08926` | RECHECK PASS | 135 |
| `dbd806d5aaf14ec1a668065e61fb985e` | RECHECK PASS | 149 |

Output native: `rechecked_20261027T111127/`, summary `samples=2 passed=2 failed=0`.
Công cụ đã remux bản sao, full decode và kiểm tra PTS/DTS; mẫu gốc giữ nguyên.
Người dùng sau đó chạy lại bài HTTP 180 giây: **11 EVENT START, 11 SEND READY,
0 SEND FAILED**, session `20261027T041336.625566Z_pid27179`. Đây là xác nhận đường gửi/ACK một
camera sau sửa, chưa xác nhận chất lượng hình ảnh đầy đủ hoặc tải nhiều camera.

Người dùng báo VLC trên Optiplex có chỗ trắng màn một frame. Qua SSH đã thấy
PTS trùng trong các clip nhận, dù nội dung giải mã không có frame trắng toàn ảnh.
Đã quét bản nguồn `15.mp4` ở nhịp 20 fps (1.201 frame, scale 320 để đo luma):
không có frame có YAVG >=230; YAVG lớn nhất 142,118. Đây chỉ là phép tìm trắng
toàn ảnh trên bản nguồn hiện có, không thay đối chiếu clip nhận hoặc xác nhận
không có bất kỳ lỗi hình ảnh nào.

## Xác nhận sau sửa metadata fragment

Đã bổ sung `fmp4_timing.py` và kiểm tra PTS duy nhất/DTS tăng nghiêm ngặt trước
READY. Bộ HTTP mới PASS **9 tình huống tích hợp** (thêm jitter 0,5 ms để tái hiện
lỗi default duration), **12 test receiver** và **3 test metadata fragment**.
Chạy trên WSL với đoạn 1080p 80 frame lấy từ `15.mp4`; đối chiếu picture NAL,
PTS/DTS/keyframe của clip đầy đủ, clip sự kiện và clip có jitter. 15 test Python
cũng PASS trên Optiplex Ubuntu 24.04.

Hai mẫu lỗi thật được sửa trên bản sao: 135 và 149 packet, không PTS trùng, DTS
tăng nghiêm ngặt, full decode đạt. SHA-256 của payload `mdat` trước/sau sửa
metadata giống nhau ở cả hai mẫu.

Bài native mới: Firefly run `20261027T115856_pid35626`, session
`20261027T045857.235568Z_pid35666`. 12/12 sự kiện gửi thành công, process failures
0, khoảng 20 fps. Cả 12 clip/2.828 packet giải mã thành 2.828 frame; không lỗi
decoder, không PTS trùng, không DTS không tăng, không frame YAVG >=230. Có một
khoảng timestamp 262 ms ở clip 3, khớp khoảng nhảy nguồn encoded 261,819 ms trong
log Firefly; không tự san timestamp nguồn. Chưa xác nhận trực quan bằng VLC sau
sửa hoặc chạy nhiều camera/dài ngày.

Xem [TROUBLESHOOT_HTTP_422.md](TROUBLESHOOT_HTTP_422.md) cho quy trình lấy mẫu lỗi.
