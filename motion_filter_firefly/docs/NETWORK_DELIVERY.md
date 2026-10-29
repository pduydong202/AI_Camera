# Gửi sự kiện trực tiếp tới Optiplex

Bản thử dùng Firefly RK3588 để lọc chuyển động; Optiplex Ubuntu 24.04 nhận và lưu
video sự kiện. L1/L2/L3, điều kiện VALID và pre/post-roll giữ nguyên. Không cần IP
cố định trong mã: đổi `VIDEO_SERVER_URL` khi chuyển LAN, VPN hoặc HTTPS.

## 1. Chạy bộ nhận trên Optiplex

Chép repository lên Optiplex, rồi từ thư mục gốc repository:

```bash
sudo apt-get update
sudo apt-get install -y python3 ffmpeg curl gstreamer1.0-tools \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
cd event_receiver
export VIDEO_SERVER_TOKEN="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
python3 server.py --host 0.0.0.0 --port 8080 --output ./received
```

Giữ terminal này chạy. Lưu token vừa tạo để khai báo cùng giá trị trên Firefly;
không đưa token thật lên Git. Lấy IP LAN Optiplex bằng `hostname -I`. Nếu firewall
đang bật, cho phép TCP 8080 từ địa chỉ Firefly hoặc interface VPN.

Từ terminal khác:

```bash
curl --fail http://127.0.0.1:8080/health
```

Kết quả: `{"status": "ok"}`. Chạy cùng URL bằng IP Optiplex từ Firefly để kiểm tra
đường mạng. `/health` chỉ kiểm tra dịch vụ; không thay kiểm tra upload/đĩa trống.

## 2. Cấu hình Firefly

Cài dependency mới `libcurl4-openssl-dev`; giữ nguyên MPP/RGA/OpenCV đã có:

```bash
sudo apt-get install -y libcurl4-openssl-dev
cd motion_filter_firefly
```

Thêm vào `.env` hiện có (không chép đè URL camera):

```dotenv
VIDEO_SERVER_URL="http://<IP_OPTIPLEX>:8080"
VIDEO_SERVER_TOKEN="<CUNG_TOKEN_VOI_BO_NHAN>"
```

Trong `config.yaml`, mặc định mới là `record_destination: "http"`. Đặt
`record_device_id` khác nhau cho mỗi thiết bị Firefly; camera id đã nằm trong
`cameras`. Giữ `validation_enabled: 0`, `l2_capture_enabled: 0` khi gửi không ghi
video đĩa. Loader từ chối bật hai chế độ chẩn đoán đó cùng HTTP.

```bash
bash build.sh
bash run.sh --dry-run
bash run.sh --seconds 180
```

Quan sát trên Firefly:

```text
[EVENT START] ...
[SEND START] event=1 destination=http ...
[SEND READY] event=1 confirmed_by_server=1
```

`EVENT STOP` là mốc thuật toán đóng sự kiện; chỉ `SEND READY` xác nhận máy chủ đã
nhận clip hoàn chỉnh. `SEND FAILED` ghi lỗi truyền/hết bộ đệm; lỗi mạng không làm
worker phân tích kết thúc. Sự kiện tiếp theo mở kết nối mới, không replay toàn bộ
sự kiện lỗi. Log/CSV vẫn ghi trên Firefly; không tạo clip hoặc video tạm ở HTTP mode.

Khi writer lỗi, xem `[SEND DIAG]` để phân biệt upload, muxer, appsrc và giới hạn
byte; log có số byte đang chờ trong từng lớp. `[SEND DETAIL]` giữ chi tiết lỗi
gốc sau cleanup. Lỗi curl có thêm cổng TCP phía client, thời gian kết nối và số
byte đã gửi để đối chiếu `[ACCEPT]`/`[UPLOAD START]` trên receiver. `[BUSY]` nghĩa
là đã chạm giới hạn upload đang xử lý. Hàng đợi TCP chờ accept được đặt riêng,
tối thiểu 16 hoặc gấp đôi `--max-uploads` (mặc định 32); không tăng số upload
được xử lý đồng thời vượt giới hạn đã cấu hình.

## 3. Kiểm tra video trên Optiplex

Đầu ra:

```text
received/<device>/<camera>/<session>/<event>/
  clip.mp4
  event.json
```

Metadata gồm định danh, thời gian nhận UTC, byte count, SHA-256 (riêng upload và
clip cuối) và kết quả ffprobe. Optiplex remux MP4 phân mảnh sang MP4 thông thường
bằng GStreamer, giữ nguyên video nén, không encode. Cách này tương thích với cả
FFmpeg cũ gặp lỗi đọc offset của các fragment do GStreamer 1.20 tạo.
Trước remux, `fmp4_timing.py` khôi phục default duration/size bị thiếu từ giá trị
đã ghi trong chính fragment; không suy timestamp từ FPS. Sau remux, bộ nhận
kiểm tra PTS duy nhất và DTS tăng nghiêm ngặt. Xem [TIMESTAMP_REPAIR.md](TIMESTAMP_REPAIR.md).
Chỉ công bố thư mục sự kiện sau khi upload xong, kiểm tra cấu trúc MP4 và probe
video H.264/H.265 thành công. Clip/metadata được công bố cùng nhau bằng rename
thư mục trên cùng filesystem. Không có bước AI trong bản thử.

Thử giải mã một clip (điền đường dẫn thật):

```bash
ffprobe -v error -show_streams -show_format received/<device>/<camera>/<session>/<event>/clip.mp4
ffmpeg -v error -xerror -i received/<device>/<camera>/<session>/<event>/clip.mp4 -f null -
```

Máy chủ probe metadata mỗi sự kiện, không decode toàn bộ tại runtime. Khi test,
cần xem clip với chuyển động có mốc rõ ràng để kiểm tra pre/post-roll 2 giây và
độ khớp sự kiện. Pre-roll phụ thuộc keyframe/GOP và lịch sử còn trong RAM.

## 4. Test tự động và test mạng

Trên Linux development (không cần MPP/RGA cho test HTTP):

```bash
sudo apt-get install -y g++ pkg-config libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad libcurl4-openssl-dev python3 ffmpeg
cd motion_filter_firefly
bash tools/check_http_sender.sh
```

Bài tích hợp dùng fixture video thật: gửi C++/GStreamer/libcurl tới bộ nhận HTTP,
decode toàn clip và so sánh packet nén/PTS/DTS/keyframe với nguồn. Có kiểm tra
pre/post-roll qua EventManager/keyframe ring, sai token, server trả 503 rồi sự
kiện sau phục hồi, server không phản hồi, giới hạn RAM và không tạo thư mục
video local. Bộ test riêng cho receiver kiểm tra body
fixed/chunked, duplicate, auth, path, giới hạn byte và upload/MP4 bị cắt.

Trên thiết bị thật, chạy lần lượt:

1. Một camera trong LAN, xem ít nhất vài sự kiện và đối chiếu timeline.
2. Dừng bộ nhận giữa sự kiện rồi bật lại; sự kiện lỗi không được báo READY,
   sự kiện mới gửi bình thường. Quan sát FPS, RSS và log.
3. Cho Firefly dùng hotspot, Optiplex giữ mạng cũ, kết nối qua VPN rồi lặp lại.
4. Tăng số camera dần và đo CPU/RAM/FPS/băng thông; số đo tải cũ với recorder file
   không xác nhận hiệu năng của đường HTTP mới.

## 5. Tự chạy bằng systemd trên Ubuntu

Ví dụ từ thư mục gốc repository trên Optiplex:

```bash
sudo mkdir -p /opt/ai-camera/event_receiver
sudo install -m 644 event_receiver/server.py event_receiver/fmp4_timing.py /opt/ai-camera/event_receiver/
sudo install -m 600 event_receiver/receiver.env.example /etc/motion-event-receiver.env
sudo nano /etc/motion-event-receiver.env
sudo install -m 644 event_receiver/motion-event-receiver.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now motion-event-receiver
sudo journalctl -u motion-event-receiver -f
```

Điền token thật trong file env trước khi start. Unit dùng DynamicUser và thư mục
state `/var/lib/motion-event-receiver`; dữ liệu tồn tại qua restart. Cần quản lý
dung lượng/retention riêng, chưa tự xóa sự kiện hoàn chỉnh. SIGKILL/mất điện ở bộ
nhận có thể để lại thư mục `.part`; chúng không được coi là sự kiện hoàn chỉnh.

## 6. Giao thức và giới hạn

- `PUT /v1/events/<device>/<camera>/<session>/<event>.mp4`, body `video/mp4`, HTTP/1.1
  chunked. `Authorization: Bearer <token>`; thành công duy nhất HTTP 201 sau hoàn tất.
- `GET /health` không cần token. 409 từ chối ghi đè, 413 quá dung lượng, 422 video
  không qua kiểm tra. Upload lỗi bị xóa; ngắt kết nối sau commit có thể khiến
  Firefly chưa biết clip đã nhận, nên không tự retry cùng event.
- MP4 phân mảnh, giữ payload H.264/H.265 gốc, không decode/encode lại để gửi.
  Đường HTTP không dùng `faststart` vì cơ chế đó có thể ghi file tạm.
- `record_ring_max_mib` (32): trần ring packet nén. `writer_queue_max_mib` (32):
  trần byte packet chờ writer; `writer_queue_max_messages` vẫn là trần số message.
  `http_buffer_max_mib` (8): trần riêng cho appsrc và queue byte HTTP, chưa tính
  bộ nhớ nội bộ parser/muxer, thư viện mạng và kernel socket.
- `http_fragment_ms` (1000), `http_timeout_sec` (5): thời gian fragment, timeout kết
  nối/thiếu dữ liệu/tốc độ thấp và chờ xác nhận. Không đảm bảo giao đủ khi mạng
  mất lâu; không có file trên Firefly để replay. Quá tải làm rơi sự kiện và có log.
  Khi queue thiếu packet/Stop, các sự kiện tồn đọng tới ID lỗi cũng bị bỏ để phục
  hồi nhanh; log có `discard_pending_through`. Khi shutdown, backlog vượt thời
  gian còn lại cũng bị bỏ, không chờ vô hạn.
  Clip lớn/remux chậm có thể cần tăng timeout cùng deadline shutdown; trường hợp
  receiver đã commit nhưng ACK tới muộn vẫn có thể được Firefly ghi là lỗi.
- Bộ nhận mặc định tối đa 16 kết nối đồng thời, 512 MiB/sự kiện, 3600 giây/upload,
  idle timeout 15 giây. Chỉnh bằng tham số `server.py --help` khi cần.
  Gói evidence trên Firefly không chứa clip ở HTTP mode; cần lấy riêng video và
  metadata trên Optiplex khi đối chiếu chất lượng/sự kiện.
  Khi cần chẩn đoán upload bị từ chối, có thể bật `--keep-failed-count 2
  --keep-failed-max-mib 32` trên bộ nhận. Mẫu có đuôi `.part` trong `_failed`,
  không được công bố là clip thành công; mặc định không giữ mẫu.
- Bộ nhận stdlib là dịch vụ thử LAN/VPN. Khi đưa API ra Internet, đặt sau reverse
  proxy HTTPS với xác thực/giới hạn phù hợp; tắt request buffering cho streaming.
  Libcurl kiểm tra chứng chỉ HTTPS theo mặc định, không tắt TLS verification.
- Chuyển mạng: thay `VIDEO_SERVER_URL` bằng IP/tên VPN, hoặc HTTPS URL có route
  tới Optiplex. HTTP qua LAN không mã hóa; VPN bảo vệ lưu lượng khi dùng khác mạng.
- Đã xác nhận một nguồn RTSP H.264 trên Firefly → Optiplex Ubuntu 24.04, bài
  180 giây đạt 12/12 sự kiện sau sửa timestamp. Chưa xác nhận nhiều camera,
  chạy dài ngày hoặc H.265 native; fixture tích hợp là H.264.

Lịch sử lỗi remux 422 và kết quả native mới nằm trong
[NETWORK_VERIFICATION.md](NETWORK_VERIFICATION.md). Khi cập nhật bộ nhận, luôn
chép `server.py` cùng `fmp4_timing.py`; không cần build lại Firefly cho sửa này.
