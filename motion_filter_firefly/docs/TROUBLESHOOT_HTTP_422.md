# Chẩn đoán 422 khi Optiplex remux MP4

**Trạng thái hiện tại:** đã sửa đường remux và metadata duration trong fragment;
bài native mới đạt 12/12 sự kiện, 2.828 frame giải mã được, không PTS trùng.
Các bước chẩn đoán bên dưới ghi lại lỗi ban đầu. Đọc
[TIMESTAMP_REPAIR.md](TIMESTAMP_REPAIR.md) để xem nguyên nhân và kết quả mới.
`server.py` hiện phụ thuộc `fmp4_timing.py`: phải cập nhật cả hai file.

Bài native `20261027T103329_pid19368`: Firefly phát hiện 13 sự kiện; Optiplex nhận
hoàn chỉnh 2 sự kiện (9, 13), từ chối 11 sự kiện với `422 fragmented MP4 remux
failed`. Log ban đầu chưa chứa stderr/mã thoát GStreamer. Log chẩn đoán tiếp theo
đã xác định lỗi `Buffer has no PTS` ở `mp4mux` trong bộ nhận. Không coi bài native
này là đã đạt gửi video.

Bản chẩn đoán thêm stderr/mã thoát vào `[REJECTED]` và tùy chọn giữ mẫu lỗi trên
Optiplex. Chỉ cần cập nhật bộ nhận để lấy thông tin; không cần build lại Firefly
ở bước này. Không thay thuật toán, ngưỡng, timeout hoặc phương thức truyền.

## Sửa đường remux và kiểm tra ngay hai mẫu cũ

Đường cũ: `qtdemux → h264parse/h265parse → mp4mux`. Đường cập nhật:
`qtdemux.video_0 → mp4mux`. MP4 demuxer đã xuất các access unit cùng PTS/DTS từ
container; không cần parse lại bitstream ở bước remux này.
[mp4mux nhận AVC/AVC3 và HVC1/HEV1](https://gstreamer.freedesktop.org/documentation/isomp4/mp4mux.html).
Việc bỏ parser tránh một bước có thể xóa PTS; log hiện tại chưa chứng minh chính
xác timestamp bị mất ở pad nào, nên cần xác nhận bằng mẫu đã lưu trên Optiplex.
Không có bước đặt lại FPS, tự sinh timestamp hay bỏ frame thiếu timestamp.

Ctrl+C receiver, rồi trong PowerShell Windows:

```powershell
scp "D:\AI_Camera\event_receiver\server.py" "D:\AI_Camera\event_receiver\fmp4_timing.py" "D:\AI_Camera\event_receiver\recheck_failed.py" manhinhtrungtam@192.168.1.243:~/AI_Camera_http_test_20261027/event_receiver/
```

Trong SSH Optiplex, kiểm tra các mẫu lỗi **trên bản sao**, chưa cần chạy Firefly:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
python3 recheck_failed.py --output "rechecked_$(date +%Y%m%dT%H%M%S)"
```

Kết quả cần có `[RECHECK SUMMARY] samples=2 passed=2 failed=0`. Mỗi mẫu được remux
trên bản sao, FFmpeg giải mã hết, kiểm tra PTS duy nhất và DTS tăng nghiêm ngặt. Mẫu gốc
được đối chiếu SHA-256 để đảm bảo không bị sửa. Kết quả nằm trong thư mục
`rechecked_<time>`, không bị công bố thành sự kiện đã nhận thành công.

Chỉ sau khi cả hai mẫu đạt, bật lại receiver với token cũ, rồi chạy lại bài
Firefly 180 giây để xác nhận số sự kiện gửi thành công. Nếu vẫn `RECHECK FAILED`,
lấy dòng lỗi mới và chép `_failed` về Windows theo lệnh bên dưới để phân tích dữ
liệu/timestamp trực tiếp. Chưa đánh dấu lỗi native đã giải quyết trước bước này.

Đã nhận kết quả recheck native từ người dùng: hai mẫu `84b148cc...` và `dbd806d5...`
đều PASS, lần lượt 135 và 149 packet; `samples=2 passed=2 failed=0`. Bước còn lại
là bài HTTP trực tiếp sau sửa để xác nhận các sự kiện đều nhận được. Người dùng
đã chạy bài này và báo **11 sự kiện / 11 gửi thành công / 0 lỗi gửi**. Khi điều
tra báo cáo VLC trắng một frame, đã tìm thấy PTS trùng trong clip dù gửi thành
công; sửa metadata fragment và kết quả chạy lại nằm trong TIMESTAMP_REPAIR.md.

## Cập nhật bộ nhận

1. Ctrl+C bộ nhận đang chạy trên Optiplex. Giữ nguyên SSH/token đang sử dụng.
2. Trong PowerShell Windows, chép file đã cập nhật:

```powershell
scp "D:\AI_Camera\event_receiver\server.py" "D:\AI_Camera\event_receiver\fmp4_timing.py" manhinhtrungtam@192.168.1.243:~/AI_Camera_http_test_20261027/event_receiver/
```

3. Trong SSH Optiplex:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
python3 server.py --host 0.0.0.0 --port 8080 --output ./received \
  --keep-failed-count 2 --keep-failed-max-mib 32 2>&1 | tee -a receiver_diag_422.log
```

Nếu dùng SSH mới, export lại `VIDEO_SERVER_TOKEN` bằng **token cũ** trước khi chạy.
Chế độ chẩn đoán giữ tối đa 2 mẫu, tổng không quá 32 MiB kể cả các mẫu đã giữ
trước lần restart. Mẫu lỗi nằm trong `received/_failed/<id>/`, gồm
`upload.mp4.part`, `failure.json` và có thể có `normalized.mp4.part`. Chúng không
được công bố thành clip/event hoàn chỉnh. Khi hết ngân sách sẽ có `DIAG SKIPPED`.
Mặc định không bật tùy chọn này thì vẫn xóa upload lỗi như bản trước.

## Chạy lại ngắn và lấy lỗi thật

Giữ MediaMTX và FFmpeg chạy như trước. Trên Firefly:

```bash
cd ~/AI_Camera_http_test_20261027/motion_filter_firefly
bash run.sh --seconds 45
```

Trên một SSH Optiplex khác:

```bash
cd ~/AI_Camera_http_test_20261027/event_receiver
grep -E 'REJECTED|DIAG|READY' receiver_diag_422.log
gst-launch-1.0 --version
ffprobe -version | head -n 1
```

Cần lấy dòng có dạng `detail="exit=... stderr=..."`. Nó phân biệt được lỗi caps,
timestamp/parser, plugin hoặc timeout subprocess. Chưa chọn cách sửa chỉ từ
thông báo tổng quát `remux failed`.

Nếu cần phân tích mẫu trên Windows, sau khi có `DIAG SAVED`:

```powershell
scp -r manhinhtrungtam@192.168.1.243:~/AI_Camera_http_test_20261027/event_receiver/received/_failed "D:\AI_Camera\motion_filter_firefly\build_deploy\received_failed_samples"
```

## Xác nhận bản chẩn đoán

Test WSL Ubuntu 22.04: 7 tình huống sender/receiver và 10 test receiver đạt. Có
kiểm tra phản hồi 422 chứa nguyên nhân remux, mã thoát/stderr, che token được
receiver phản hồi lại, và ngân sách giữ mẫu lỗi theo số lượng/byte.

Đã thử thêm với đoạn 1080p 4 giây lấy từ `15.mp4`, mã hóa H.264 20 fps, ultrafast,
zerolatency, 2 Mbps, GOP 20 như lệnh phát. Các clip thành công qua full decode và
đối chiếu picture/PTS/DTS/keyframe (80/91 packet). Chưa tái hiện được lỗi remux
native trên WSL; vẫn cần stderr và mẫu từ Optiplex để sửa nguyên nhân thực tế.

Sender C++ cũng đã được bổ sung mã HTTP và body lỗi có giới hạn/che token. Phần
đó chỉ có tác dụng khi cập nhật và build lại Firefly; không bắt buộc để thu lỗi
phía Optiplex theo các bước trên.

Bản remux trực tiếp đã qua các bài HTTP với video 1080p và đối chiếu picture/
PTS/DTS/keyframe 80/91 packet trên WSL. Công cụ recheck được kiểm tra không sửa
mẫu gốc, không ghi đè output cũ, full decode và không công bố mẫu lỗi là READY.
