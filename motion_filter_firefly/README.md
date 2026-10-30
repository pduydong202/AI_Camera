# motion_filter — Firefly RK3588

Bộ lọc chuyển động dành cho **Firefly RK3588**, sử dụng giải mã H.264/H.265 bằng MPP, thu nhỏ NV12 bằng RGA, xử lý thuật toán L1/L2/L3 trên CPU và ghi lại clip sự kiện từ luồng nén gốc.

Dự án dùng **C++17 và Bash** trên Firefly. Bộ nhận thử nghiệm trên Optiplex dùng Python 3, FFmpeg và GStreamer; test tích hợp HTTP cũng dùng Python.

**Đầu ra mặc định hiện tại:** gửi video sự kiện trực tiếp qua HTTP tới Optiplex,
giữ pre/post-roll trong RAM, không tạo file video trên Firefly. Cài đặt và kiểm
thử hai máy theo [NETWORK_DELIVERY.md](docs/NETWORK_DELIVERY.md). Các số đo tải và
bằng chứng ngày 15/10 dưới đây thuộc đường recorder file trước khi thêm HTTP.

Hệ thống hướng tới xử lý nhiều camera RTSP, giảm lượng video không cần thiết phải truyền tới máy chủ AI bằng cách chỉ lưu các đoạn video có sự kiện chuyển động.

## 1. Tổng quan

Hệ thống gồm các thành phần chính:

| Thành phần | Chức năng |
|---|---|
| MPP | Giải mã phần cứng H.264/H.265 |
| RGA | Thu nhỏ khung hình NV12 |
| L1 | Phát hiện chuyển động bằng MOG2 và lọc nhiễu |
| L2 | Giảm chuyển động lặp lại theo không gian và thời gian |
| L3 | Lọc chuyển động không hợp lệ bằng TAWL |
| Event Recorder | Ghi clip từ luồng video nén gốc |
| Supervisor | Quản lý tiến trình camera, watchdog và reconnect |
| Monitoring | Theo dõi FPS, CPU, RAM, nhiệt độ và trạng thái xử lý |

Cấu hình được quản lý tập trung:

- `config.yaml`: tham số thuật toán, vận hành và danh sách camera.
- `.env.example`: mẫu khai báo URL RTSP.
- `.env`: cấu hình URL RTSP thực tế, không được đưa lên GitHub.

## 2. Yêu cầu hệ thống

**Phần cứng:**
- Firefly sử dụng SoC Rockchip RK3588/RK3588S.
- Hỗ trợ giải mã video qua Rockchip MPP.
- Hỗ trợ xử lý ảnh bằng Rockchip RGA.

**Phần mềm:**
- Linux ARM64 (môi trường đã sử dụng: Ubuntu 22.04).
- CMake và trình biên dịch hỗ trợ C++17.
- pkg-config.
- OpenCV development files.
- GStreamer và plugin `rockchipmpp`.
- Header và thư viện `librga`.
- Header và thư viện `libcurl` (`libcurl4-openssl-dev`).
- Bash, GNU coreutils, procps, util-linux, awk và tar.

FFmpeg/ffprobe và Python 3 là bắt buộc cho test tích hợp HTTP. Một số bước kiểm
tra media cũ trong collector vẫn được bỏ qua khi thiếu FFmpeg; điều đó không
thay cho test HTTP đầy đủ.

Giữ thư viện MPP/RGA của thiết bị. `build.sh` build plugin GStreamer Rockchip đã
sửa deinit vào `build_gst/`; `run.sh` chọn plugin riêng, không thay `/usr/lib`.
Cần thêm Python 3.10+, Ninja và mạng ở lần build đầu. Xem
[quy trình plugin riêng](docs/ROCKCHIP_PLUGIN.md).

## 3. Cài đặt và chạy trên Firefly

Clone repository:

```bash
cd ~

git clone https://github.com/pduydong202/AI_Camera.git

cd ~/AI_Camera/motion_filter_firefly
```

Tạo file cấu hình môi trường từ file mẫu:

```bash
cp .env.example .env
nano .env
```

Chỉnh URL RTSP theo nguồn camera đang sử dụng.

**Biên dịch:**

```bash
bash build.sh
```

**Chạy bộ kiểm thử:**

```bash
bash check.sh
```

**Kiểm tra cấu hình trước khi chạy:**

```bash
bash run.sh --dry-run
```

`--dry-run` kiểm tra cấu hình và URL đã khai báo mà chưa mở kết nối camera.

**Chạy hệ thống:**

```bash
bash run.sh
```

Chương trình chạy các camera được khai báo trong `config.yaml` cho đến khi nhấn `Ctrl+C`.

Khi dừng, chương trình thực hiện quy trình đóng clip và thu thập thông tin kiểm tra. Nếu worker buộc phải kết thúc, một số clip có thể chưa được hoàn tất.

Không cần chạy toàn bộ self-test trước mỗi lần sử dụng.

## 4. Cấu hình nhiều camera

Các camera được quản lý trong cùng một file `config.yaml`.

Ví dụ:

```yaml
cameras:
  - { id: cam01, url_env: CAM01_RTSP, codec: h264, gop_hint_sec: 0.0 }
  - { id: cam02, url_env: CAM02_RTSP, codec: h264, gop_hint_sec: 0.0 }
```

Trong file `.env`, khai báo URL tương ứng:

```dotenv
CAM01_RTSP="rtsp://192.0.2.10:8554/cam01"
CAM02_RTSP="rtsp://192.0.2.11:554/stream"
```

Các địa chỉ trên chỉ là ví dụ, không phải địa chỉ camera thực tế.

Sau khi cấu hình, chạy:

```bash
bash run.sh
```

Mỗi camera có một supervisor và tối đa một worker hoạt động tại cùng thời điểm.

Khi camera lỗi hoặc mất kết nối, watchdog có thể khởi động lại worker theo chính sách reconnect được khai báo trong YAML.

**Lưu ý về `.env`:**

Bộ đọc `.env` xử lý giá trị dưới dạng văn bản, không thực thi lệnh shell, không mở rộng biến `$` và không sử dụng `source`.

URL có thể được đặt trong dấu nháy đơn hoặc đôi. Không thêm bình luận ở cuối dòng URL.

Ký tự đặc biệt trong tên đăng nhập hoặc mật khẩu phải được mã hóa phù hợp với định dạng URI.

Log ứng dụng che phần user/password của URI. Tuy nhiên, URL vẫn có thể xuất hiện trong danh sách tham số tiến trình của hệ điều hành.

## 5. Thuật toán phát hiện chuyển động

Hệ thống xử lý theo ba lớp chính.

### L1 — Phát hiện chuyển động

Sử dụng MOG2 để phát hiện foreground và các phép xử lý hình thái học nhằm loại bỏ nhiễu nhỏ.

Cấu hình mặc định:

- Độ phân giải phân tích tối đa: 854×480.
- FPS mục tiêu: 20.
- MOG2 history: 500.
- MOG2 variance threshold: 16.
- Background learning: tự động.
- Morphological OPEN: kernel 3×3.
- Connected components: 8-connectivity.

### L2 — Giảm chuyển động lặp

Sử dụng lịch sử chuyển động theo MOG2 và phương pháp Otsu 8-bit nhằm giảm các vùng chuyển động lặp lại.

Ví dụ: lá cây rung, quạt hoặc những chuyển động lặp tại một khu vực cố định.

L2 không đảm bảo loại bỏ hoàn toàn mọi chuyển động nền lặp.

### L3 — Lọc chuyển động bằng TAWL

Sử dụng các ngưỡng không gian và thời gian:

- `TA = 0.20 × analysis_fps`.
- `W = 0.05 × analysis_width`.

Các thông số này được áp dụng theo cấu hình phân tích thay vì phụ thuộc trực tiếp vào độ phân giải gốc của camera.

Phần xử lý sparse L3 đã có bài kiểm thử đối chiếu với thuật toán tham chiếu nhằm bảo toàn kết quả khi tối ưu CPU.

### Xác nhận sự kiện

Thông số mặc định:

| Tham số | Giá trị |
|---|---|
| Warmup | 2 giây |
| VALID area ratio | ≥ 0.005 |
| Pre-roll | 2 giây |
| Post-roll | 2 giây |
| Analysis FPS | 20 FPS mục tiêu |

Khi phát hiện chuyển động hợp lệ, hệ thống tạo clip sự kiện bao gồm khoảng thời gian trước và sau sự kiện.

Clip dùng luồng video nén gốc. Ở chế độ HTTP, Firefly gửi MP4 phân mảnh; Optiplex
remux thành MP4 thông thường, không encode lại. Chế độ `record_destination: "file"`
vẫn có để chạy đối chiếu recorder cũ.

Thời gian pre-roll thực tế còn phụ thuộc vào keyframe và GOP của nguồn video.

Tài liệu chi tiết: [ALGORITHM.md](docs/ALGORITHM.md).

## 6. Quản lý cấu hình

Các tham số xử lý và vận hành được khai báo trong `config.yaml`, bao gồm:

- Cấu hình MOG2.
- Kernel và connectivity.
- Lịch sử xử lý L2.
- Tham số TAWL.
- Kích thước và FPS phân tích.
- Ngưỡng xác nhận sự kiện.
- CPU affinity.
- Giới hạn queue GStreamer.
- Timeout của source và writer.
- Watchdog và reconnect.
- Logging và monitoring.
- Giới hạn clip mẫu và dung lượng chứng cứ.

Loader kiểm tra tính đầy đủ, kiểu dữ liệu, khóa trùng, khóa không hợp lệ và các giá trị không đúng phạm vi.

Một số hằng số biểu diễn dữ liệu vẫn được giữ trong mã nguồn, chẳng hạn:

- Mask nhị phân 0/255.
- Otsu 8-bit.
- Cấu trúc NV12.
- Căn chỉnh stride theo ABI RGA.
- Định dạng capture và CRC.

Đây là những thành phần định nghĩa dữ liệu hoặc thuật toán, không phải các ngưỡng vận hành cần chỉnh riêng cho từng camera.

Thử nghiệm L2 16-bit chưa được tích hợp vì đã quan sát thấy hiện tượng mất tín hiệu xe trong quá trình đánh giá.

**Lưu ý:** Thay đổi tham số thuật toán trong YAML có thể làm thay đổi kết quả phát hiện và cần được kiểm thử lại bằng video thực tế.

Tài liệu chi tiết: [CONFIGURATION.md](docs/CONFIGURATION.md).

## 7. Kiểm thử hiệu năng trên RK3588

Các phép thử được thực hiện trên Firefly RK3588 với luồng video RTSP mô phỏng.

| Phép thử | Analysis FPS/kết nối | CPU toàn máy | Kết quả |
|---|---|---|---|
| 8 kết nối, 30 phút, main CPU 4–7 | ~20.00 | ~52.16% | Đạt FPS mục tiêu |
| 15 kết nối, 10 phút, main CPU 4–7 | 14.23–14.92 | ~80.87% | Không đạt 20 FPS |
| 15 kết nối, 3 phút, main không ghim | 13.77–14.67 | ~99.19% | Không cải thiện |

**Kết luận từ các phép thử:**

- Cấu hình main CPU affinity 4–7 được giữ làm mặc định đã kiểm thử.
- 8 kết nối là mốc tải đã đo đạt khoảng 20 FPS phân tích mỗi kết nối.
- 10 kết nối chưa được xác nhận đạt yêu cầu.
- 15 kết nối không duy trì được FPS mục tiêu trong các phép thử đã thực hiện.

Các phép thử sử dụng nhiều kết nối tới cùng một nguồn RTSP mô phỏng, không phải 8 hoặc 15 camera vật lý độc lập.

Hiệu năng thực tế còn phụ thuộc vào độ phân giải, bitrate, FPS, GOP và mức độ chuyển động trong khung hình.

Các phép thử có thời lượng khác nhau, chưa đủ để kết luận về rò rỉ bộ nhớ, độ ổn định nhiệt hoặc độ tin cậy khi hoạt động liên tục 24/7.

### Kiểm thử nhiều kết nối

Để thử tải với 8 kết nối tới cùng một nguồn:

```bash
bash run.sh --replicas 8 --seconds 180
```

`--replicas` chỉ sử dụng được khi YAML khai báo một camera. Đây là chế độ mô phỏng tải, không phải phương pháp khai báo nhiều camera thực tế.

`--seconds 0` hoặc không khai báo `--seconds` nghĩa là chạy liên tục.

### Trạng thái xác nhận

Bản gom mã và cấu hình đã qua kiểm thử trên môi trường phát triển.

Cần thực hiện build và xác nhận lại trên Firefly sau khi triển khai đúng phiên bản mã nguồn trong repository.

Chi tiết: [VALIDATION.md](docs/VALIDATION.md).

## 8. Đầu ra

Sau khi chạy, hệ thống tạo các thư mục:

Ví dụ bên dưới mô tả **chế độ file**. Với mặc định HTTP, `events.csv` và log vẫn
nằm trên Firefly, còn video/metadata nằm trong `received/<device>/<camera>/<session>/<event>/`
trên Optiplex; không có clip local. Xem [hướng dẫn HTTP](docs/NETWORK_DELIVERY.md).

```text
motion_filter_firefly/
├── output/
│   └── <run_id>/
│       └── <camera>/
│           └── sessions/
│               └── <session_id>/
│                   ├── clips/
│                   │   ├── event_000001.mp4
│                   │   └── ...
│                   └── events.csv
│
├── logs/
│   └── <run_id>/
│
└── evidence/
    └── <run_id>.tar.gz
```

### Clip sự kiện

Video được ghi từ luồng nén gốc, không encode lại.

File đang ghi sử dụng đuôi `.mp4.part`.

Sau khi hoàn tất EOS thành công, file mới được đổi tên thành `.mp4`.

`gop_hint_sec: 0` cho phép học GOP quan sát được từ luồng camera.

### Log và dữ liệu giám sát

Thư mục `logs/` lưu thông tin phục vụ chẩn đoán:

- FPS phân tích.
- CPU và nhiệt độ.
- RSS của supervisor và worker.
- Cấu hình phiên chạy.
- Hash mã nguồn, script và binary.
- Thông tin clip.
- Báo cáo kiểm tra.

Dòng `cameras_with_perf=8/8` chỉ cho biết cả 8 log đã xuất hiện bản ghi PERF; cần xem `summary.txt` để xác định FPS thực tế.

### Evidence

Thư mục `evidence/` lưu gói chứng cứ, bao gồm log, báo cáo và clip mẫu theo giới hạn YAML.

Ở chế độ file, clip đầy đủ nằm trong `output/`. Ở chế độ HTTP, video nằm trên Optiplex.

`validation_enabled` cho phép xuất thêm clip mask để kiểm tra. `l2_capture_enabled` bật capture L2 liên tục và có thể làm tăng mức sử dụng RAM hoặc đĩa.

Chế độ HTTP gửi trực tiếp sự kiện tới bộ nhận; chưa tự xóa output cũ hoặc video
đã nhận trên Optiplex. Cần thiết lập chính sách lưu trữ và quản lý dung lượng.

## 9. Quản lý tiến trình và khôi phục

Mỗi camera RTSP được quản lý bởi một supervisor.

Supervisor thực hiện:

- Khởi tạo worker.
- Giám sát trạng thái xử lý frame.
- Phát hiện mất kết nối hoặc treo.
- Dừng và thu hồi worker lỗi.
- Khởi động lại worker theo cấu hình watchdog và backoff.

Khi reconnect, hệ thống tạo session mới và khởi tạo lại trạng thái thuật toán.

Nhấn `Ctrl+C` để thực hiện quy trình dừng và hoàn tất clip.

Nếu worker bị buộc kết thúc bằng `SIGKILL`, clip đang ghi có thể còn đuôi `.part`; tình trạng này được phản ánh trong log.

## 10. Các giới hạn đã biết

### MPP

Plugin MPP trên thiết bị thử nghiệm có cảnh báo liên quan đến giải phóng frame khi dừng.

Đã đối chiếu binary với sửa đổi upstream, nhưng cảnh báo `mpp_frame_deinit` chưa được khắc phục bằng mã nguồn của dự án này.

### Thuật toán

L2 vẫn có thể bị ảnh hưởng bởi:

- Chuyển động cây rung phức tạp.
- Thay đổi ánh sáng.
- Chuyển động nền không có chu kỳ rõ ràng.

Chưa có đánh giá precision/recall đầy đủ trên một tập camera độc lập đủ lớn.

### Hiệu năng

Khi quá tải, hệ thống có thể bỏ frame phân tích và phát sinh sai lệch trong quá trình xử lý timeline PTS.

Kết quả chạy 15 kết nối không được sử dụng để khẳng định chất lượng phát hiện sự kiện tương đương với thử nghiệm 8 kết nối.

### Triển khai thực tế

Chưa xác nhận đầy đủ:

- Hoạt động liên tục 24/7 trong thời gian dài.
- Hiệu năng trên nhiều camera vật lý độc lập với nguồn RTSP khác nhau.
- Độ chính xác thuật toán trên tập video đa dạng.
- Tích hợp xử lý AI sau khi máy chủ nhận clip; bản HTTP hiện tại mới nhận và lưu.

Chi tiết: [KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

## 11. Cấu trúc mã nguồn

```text
motion_filter_firefly/
├── src/                  # Mã nguồn C++ chính
├── include/              # Header C++
├── tests/                # Kiểm thử tự động
├── tools/                # Công cụ kiểm tra và chẩn đoán
├── docs/                 # Tài liệu kỹ thuật
├── CMakeLists.txt         # Cấu hình build
├── config.yaml           # Cấu hình thuật toán và camera
├── .env.example          # Mẫu URL camera
├── .gitignore            # Quy tắc loại trừ file khỏi Git
├── build.sh              # Biên dịch
├── check.sh              # Kiểm thử
├── run.sh                # Khởi chạy
└── README.md             # Tài liệu này
```

## 12. Tài liệu bổ sung

- [Báo cáo kiểm thử theo ngày](docs/test-reports/README.md) — FPS, CPU, nhiệt độ, bộ nhớ và độ tin cậy gửi video; [bài 8 luồng/8.000 giây ngày 28/10/2026](docs/test-reports/2026-10-28-8-streams-8000s.md).
- [Algorithm](docs/ALGORITHM.md) — Logic thuật toán.
- [Configuration](docs/CONFIGURATION.md) — Các tham số cấu hình.
- [Validation](docs/VALIDATION.md) — Kết quả kiểm thử và chứng cứ.
- [Known Issues](docs/KNOWN_ISSUES.md) — Các vấn đề và giới hạn đã biết.

---

**Project:** AI_Camera / motion_filter_firefly  
**Platform:** Firefly RK3588  
**Language:** C++17, Bash  
**Video pipeline:** GStreamer, Rockchip MPP, RGA, OpenCV  
**Repository:** https://github.com/pduydong202/AI_Camera
