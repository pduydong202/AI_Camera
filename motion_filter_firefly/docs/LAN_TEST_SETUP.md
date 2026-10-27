# Chạy thử Windows → Firefly → Optiplex

Địa chỉ đã kiểm tra ngày 27/10/2026:

| Máy | Địa chỉ | Vai trò |
| --- | --- | --- |
| Windows | `192.168.1.34` | MediaMTX và FFmpeg phát video giả lập |
| Firefly | `firefly@192.168.1.248` | MPP/RGA, lọc chuyển động, gửi HTTP |
| Optiplex | `manhinhtrungtam@192.168.1.243` | Nhận HTTP và lưu video |

IP Windows có thể đổi khi kết nối Wi-Fi lại; kiểm tra bằng `ipconfig` nếu RTSP
không truy cập được. SSH/SCP cần tài khoản hoặc key đã cấu hình trên từng máy;
nhập mật khẩu trong terminal của bạn, không gửi vào chat.

## 0. Chạy lại khi hai máy đã được cấu hình

Giữ nguyên `.env` và token hiện tại. Không cần chép lại mã, cài dependency hoặc
build lại nếu chưa sửa code. Giữ MediaMTX và FFmpeg trên Windows chạy như mục 2.

Trong SSH **Optiplex**, kiểm tra bộ nhận:

```bash
curl --fail --max-time 5 http://127.0.0.1:8080/health
```

Nếu trả `{"status": "ok"}`, bộ nhận đã chạy; không chạy thêm `server.py` trên
cùng cổng. Bài kiểm thử ngày 27/10 đã để bộ nhận chạy nền; theo dõi log bằng:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
tail -f receiver_timing_fixed_20261027T045824Z.log
```

Nếu bộ nhận chưa chạy, mở một terminal SSH Optiplex và dùng **token đang cấu
hình trong `.env` trên Firefly**, không tạo token mới:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
export VIDEO_SERVER_TOKEN='<TOKEN_HIEN_TAI_TREN_FIREFLY>'
python3 server.py --host 0.0.0.0 --port 8080 --output ./received \
  --keep-failed-count 2 --keep-failed-max-mib 32
```

Giữ terminal này mở. `Address already in use` nghĩa là cổng đang có tiến trình
lắng nghe; kiểm tra lại `/health`, không khởi động thêm một bản trên cùng cổng.

Trong SSH **Firefly**, kiểm tra bộ nhận rồi chạy 180 giây khi health thành công:

```bash
cd ~/AI_Camera_http_test_20261027/motion_filter_firefly
curl --fail --max-time 5 http://192.168.1.243:8080/health
bash run.sh --seconds 180
```

Sau khi kết thúc, đếm **đúng log mới nhất**:

```bash
log=$(ls -t logs/*/cameras/cam01.log | head -n 1)
printf 'Log: %s\n' "$log"
awk '
  /\[EVENT START\]/ {events++}
  /\[SEND READY\]/  {sent++}
  /\[SEND FAILED\]/ {errors++}
  END {
    printf "Event: %d\nGui thanh cong: %d\nLoi gui: %d\n", events, sent, errors
  }
' "$log"
```

Lấy session mới từ các dòng `[READY]` trên Optiplex rồi mở clip trong
`received/firefly01/cam01/<session_moi>/<event>/clip.mp4`. Tránh lấy nhầm clip cũ
khi so sánh chất lượng. Các mục tiếp theo dành cho cài đặt từ đầu và test lỗi.

## 1. Chép đúng bản mã đang sửa

Bản mã này chưa push lên Git. Gói đã chuẩn bị trên Windows:
`D:\AI_Camera\motion_filter_firefly\build_deploy\ai-camera-http-test-20261027.tar.gz`.
Gói chỉ có mã nguồn, config mẫu, test và tài liệu; không có `.env` thật, binary,
output/log/evidence. Thư mục test mới bên dưới tách khỏi bản đang chạy của bạn.
Gói hiện đã gồm sửa metadata fragment: khi cập nhật riêng bộ nhận, chép cả
`server.py` và `fmp4_timing.py`. Kết quả native mới và clip để xem bằng VLC:
[TIMESTAMP_REPAIR.md](TIMESTAMP_REPAIR.md).

Trong PowerShell Windows:

```powershell
$bundle = "D:\AI_Camera\motion_filter_firefly\build_deploy\ai-camera-http-test-20261027.tar.gz"
scp $bundle firefly@192.168.1.248:~/
scp $bundle manhinhtrungtam@192.168.1.243:~/
```

## 2. Bật MediaMTX và phát video trên Windows

Mở MediaMTX bằng cách bạn đang dùng, giữ terminal chạy. Listener RTSP cần bind
LAN, ví dụ `rtspAddress: :8554`; không giới hạn ở `127.0.0.1:8554`. Nếu sửa config,
restart đúng phiên MediaMTX thay vì chạy thêm một phiên cùng port.

Trong một PowerShell khác, chạy nguyên lệnh phát của bạn:

```powershell
ffmpeg -re -stream_loop -1 `
    -i "D:\camera_AI_thuat_toan\video_test\15.mp4" `
    -map 0:v:0 -an -vf fps=20 `
    -c:v libx264 -preset ultrafast -tune zerolatency `
    -pix_fmt yuv420p `
    -b:v 2M -maxrate:v 2M -bufsize:v 1M `
    -g 20 -keyint_min 20 -sc_threshold 0 `
    -f rtsp -rtsp_transport tcp `
    "rtsp://127.0.0.1:8554/cam01"
```

Publisher dùng localhost vì FFmpeg và MediaMTX cùng Windows. Firefly phải đọc
`rtsp://192.168.1.34:8554/cam01`. Giữ cả MediaMTX và FFmpeg hoạt động suốt bài test.

Nếu bị Windows Firewall chặn, mở PowerShell **Run as administrator** và tạo rule
chỉ cho Firefly truy cập cổng RTSP này:

```powershell
New-NetFirewallRule -DisplayName "MediaMTX RTSP test from Firefly" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8554 -RemoteAddress 192.168.1.248 -Profile Any
```

## 3. Chạy bộ nhận trên Optiplex

Mở một PowerShell mới và đăng nhập:

```powershell
ssh manhinhtrungtam@192.168.1.243
```

Các lệnh tiếp theo chạy trong SSH **Optiplex**:

```bash
mkdir -p ~/AI_Camera_http_test_20261027
tar -xzf ~/ai-camera-http-test-20261027.tar.gz -C ~/AI_Camera_http_test_20261027
sudo apt-get update
sudo apt-get install -y python3 ffmpeg curl gstreamer1.0-tools \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
cd ~/AI_Camera_http_test_20261027/event_receiver
export VIDEO_SERVER_TOKEN="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
printf 'Token de dien vao Firefly: %s\n' "$VIDEO_SERVER_TOKEN"
python3 server.py --host 0.0.0.0 --port 8080 --output ./received
```

Sao chép token vừa hiện để điền trên Firefly, không gửi vào chat. Giữ SSH này mở.
Phải thấy `[LISTEN] 0.0.0.0:8080 ...`. Nếu khởi động lại bộ nhận trong một SSH
mới, dùng lại token đã khai báo trên Firefly thay vì tạo một token khác.

Nếu UFW đang bật và cổng 8080 bị chặn, mở thêm SSH Optiplex và chạy:

```bash
sudo ufw status
sudo ufw allow from 192.168.1.248 to any port 8080 proto tcp
```

## 4. Kiểm tra và chạy Firefly

Trong PowerShell mới:

```powershell
ssh firefly@192.168.1.248
```

Các lệnh tiếp theo chạy trong SSH **Firefly**:

```bash
mkdir -p ~/AI_Camera_http_test_20261027
tar -xzf ~/ai-camera-http-test-20261027.tar.gz -C ~/AI_Camera_http_test_20261027
sudo apt-get update
sudo apt-get install -y libcurl4-openssl-dev curl ffmpeg python3
curl --fail --max-time 5 http://192.168.1.243:8080/health
timeout 15 ffprobe -v error -rtsp_transport tcp \
  -show_entries stream=codec_name,width,height,r_frame_rate -of default=nw=1 \
  rtsp://192.168.1.34:8554/cam01
```

Health cần trả `{"status": "ok"}`; ffprobe cần thấy H.264, kích thước nguồn và
tốc độ khoảng 20 fps. Nếu một trong hai bước lỗi, sửa đường mạng/nguồn trước khi
build hoặc chạy thuật toán. MPP/RGA/OpenCV dùng bộ thư viện sẵn có của Firefly.

```bash
cd ~/AI_Camera_http_test_20261027/motion_filter_firefly
cp .env.example .env
nano .env
```

Chỉnh **ba dòng** này, thay token bằng giá trị trên Optiplex:

```dotenv
CAM01_RTSP="rtsp://192.168.1.34:8554/cam01"
VIDEO_SERVER_URL="http://192.168.1.243:8080"
VIDEO_SERVER_TOKEN="<TOKEN_VUA_TAO_TREN_OPTIPLEX>"
```

Chạy từ thư mục `motion_filter_firefly`:

```bash
bash build.sh
bash check.sh
bash run.sh --dry-run
bash run.sh --seconds 180
```

Chỉ tiếp tục sang lệnh sau khi lệnh trước thành công. `--dry-run` cần báo một
camera, không lộ token và không mở kết nối. Dùng **một camera** cho lần đầu.
Nếu launcher báo có `motion_filter` khác đang chạy, dừng phiên cũ bằng Ctrl+C ở
terminal của phiên đó rồi chạy lại; không tắt tiến trình khác một cách tùy tiện.

Sau ba phút, chương trình tự đóng sự kiện đang mở và chờ hoàn tất gửi. Xem log:

```bash
grep -E 'EVENT START|EVENT STOP|SEND START|SEND READY|SEND FAILED|ERROR' logs/*/cameras/cam01.log
```

Kết quả mong đợi có `SEND READY ... confirmed_by_server=1` cho các sự kiện gửi
thành công. `EVENT START` đơn lẻ chưa chứng minh đã gửi clip. Sự kiện chuyển động
liên tục có thể chỉ hoàn tất khi chạy hết 180 giây hoặc khi nó hết chuyển động.
Nếu không có sự kiện nào, bài test chưa kiểm chứng được việc gửi video.

## 5. Kiểm tra clip trên Optiplex

Mở thêm SSH Optiplex; terminal receiver ban đầu vẫn giữ chạy:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
find received -type f -name clip.mp4
clip=$(find "$PWD/received" -type f -name clip.mp4 -print -quit)
if [ -n "$clip" ]; then
  ffprobe -v error -show_streams -show_format "$clip"
  ffmpeg -v error -xerror -i "$clip" -f null -
fi
```

Video và `event.json` nằm trong `received/firefly01/cam01/<session>/<event>/`.
FFmpeg exit 0 và không báo lỗi là kiểm tra giải mã; vẫn cần mở xem một vài clip
để đối chiếu chuyển động và pre/post-roll. Clip dùng video gốc, không phải mask.

## 6. Thử lỗi mạng sau khi bài đầu đạt

Chạy lại Firefly trong 180 giây. Khi một sự kiện đang gửi, Ctrl+C **receiver trên
Optiplex**, đợi vài giây rồi chạy lại đúng lệnh `python3 server.py ...` trong cùng
terminal (token vẫn còn). Giữ FFmpeg/MediaMTX hoạt động. Firefly cần tiếp tục phân
tích, báo `SEND FAILED` cho phần gửi lỗi và gửi được **sự kiện mới** sau phục hồi.
Sự kiện lỗi không được tự gửi lại toàn bộ từ đĩa/RAM.

Bài này không thay bài hai mạng khác nhau: sau khi LAN đạt, dùng VPN và đổi
`VIDEO_SERVER_URL`, rồi lặp lại. Chưa tăng nhiều camera trước khi test một camera
và phục hồi lỗi mạng đạt.

Chi tiết giao thức, giới hạn và systemd: [NETWORK_DELIVERY.md](NETWORK_DELIVERY.md).
Nguồn: [cấu hình MediaMTX](https://mediamtx.org/docs/references/configuration-file).
