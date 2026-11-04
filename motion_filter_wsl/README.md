# Motion Filter — WSL

Bộ lọc chuyển động sử dụng **C++17 và OpenCV**, chạy trên Windows Subsystem for Linux (WSL). Hệ thống xử lý video theo kiến trúc **L1/L2/L3/VALID** nhằm phát hiện chuyển động, giảm nhiễu và trích xuất các đoạn video có sự kiện.

## 1. Kiến trúc thuật toán

```text
Input Video
    |
    v
L1 — MOG2 + Opening 3×3
    |
    v
L2 — Repetitive Motion Filtering
    |
    v
L3 — TAWL Rain/Noise Filtering
    |
    v
VALID — Connected Components
    |
    v
Event Detection & Recording
```

| Thành phần | Chức năng |
|---|---|
| **L1** | Phát hiện foreground bằng MOG2 và lọc nhiễu nhỏ |
| **L2** | Giảm chuyển động lặp bằng grid history và component containment |
| **L3** | Xác định bằng chứng mưa/nhiễu bằng TAWL |
| **VALID** | Xác định connected components hợp lệ |
| **Event** | Xác nhận sự kiện theo tỷ lệ diện tích VALID |

## 2. Logic xử lý

Các cơ chế chính:

- **Component TA:** Kế thừa tuổi lớn nhất (*maximum inherited age*) từ các pixel 8-neighbor của frame trước.
- **L2 Re-entry:** `reentryArmed` tự hết hạn theo cửa sổ lịch sử L2.
- **L2 Otsu:** Chuyển trực tiếp `repeatEvidence` từ `[0,1]` sang `[0,255]`, áp dụng Otsu trên toàn bộ grid, bao gồm zero cells.
- **VALID:** Connected component chưa được L2/L3 giải thích hoàn toàn được tính toàn bộ vào valid area.

Cấu hình mặc định:

| Tham số | Giá trị |
|---|---|
| Analysis resolution | 854×480 |
| Target FPS | 20 |
| Morphological kernel | 3×3 |
| TAWL | TA / W / Location |
| Event valid ratio | 0.005 |
| Pre/Post-roll | 2 giây |

Các tham số cấu hình được quản lý thông qua YAML.

## 3. Cài đặt và chạy

**Yêu cầu** gồm WSL hoặc Linux, Bash, `flock` (util-linux), `g++` hỗ trợ C++17, CMake 3.16 trở lên, `pkg-config` và thư viện phát triển OpenCV 4.

Clone repository:

```bash
git clone https://github.com/pduydong202/AI_Camera.git
cd AI_Camera/motion_filter_wsl
```

Nếu repo đã có trên ổ `D:` của Windows, mở Ubuntu trong WSL và chuyển vào thư mục dự án.

```bash
cd /mnt/d/AI_Camera/motion_filter_wsl
```

Kiểm tra môi trường và biên dịch.

```bash
bash check_wsl.sh
bash build_wsl.sh
```

Script build xác định thư mục dự án theo vị trí của chính nó. Nếu `build/CMakeCache.txt` chứa đường dẫn cũ, script chuyển thư mục build cũ vào `build_stale.XXXXXX/build` rồi tạo lại `build/` ở vị trí hiện tại. Có thể xóa bản lưu sau khi xác nhận build mới chạy được.

Chạy từ thư mục `motion_filter_wsl` với đường dẫn video trong WSL.

```bash
bash run_wsl.sh "/mnt/d/Videos/video.mp4"
```

Thay đường dẫn ví dụ bằng đường dẫn video thực tế. Script dùng `policy.yaml` nếu bạn bỏ qua đối số thứ hai. Bạn có thể truyền thêm `camera_id` sau đường dẫn policy để đặt tên thư mục camera. Mặc định chương trình lấy tên file video.

## 4. Cấu hình

| File | Độ phân giải phân tích tối đa |
|---|---|
| `policy.yaml` | 854×480 |
| `policy_360p.yaml` | 640×360 |
| `policy_720p.yaml` | 1280×720 |

Các cấu hình cho phép thử nghiệm thuật toán ở nhiều độ phân giải mà không cần sửa mã nguồn.

## 5. Kết quả đầu ra

Theo cấu hình mặc định, mỗi lần chạy lưu kết quả trong `output/<camera_id>/sessions/<session_id>/`. Thư mục này chứa `events.csv`, `frame_stats.csv`, clip sự kiện trong `clips/` và các video mask.

**Mask toàn bộ video**

```text
masks_full/
├── full_L1_foreground.mp4
├── full_L2_repeat_evidence.mp4
├── full_L3_rain_noise_evidence.mp4
└── full_VALID_motion.mp4
```

**Mask theo từng sự kiện**

```text
masks/
├── motion_XXXXXX_L1.mp4
├── motion_XXXXXX_L2.mp4
├── motion_XXXXXX_L3.mp4
└── motion_XXXXXX_VALID.mp4
```

Các VALID mask hiển thị connected components được tính là chuyển động hợp lệ. Việc xuất mask không thay đổi logic phát hiện sự kiện.

## 6. Cấu trúc dự án

```text
motion_filter_wsl/
├── src/                      # C++ source
├── include/                  # Header files
├── CMakeLists.txt
├── build_wsl.sh
├── check_wsl.sh
├── run_wsl.sh
├── policy.yaml
├── policy_360p.yaml
├── policy_720p.yaml
├── ARCHITECTURE.md
├── AUDIT_NO_CAMERA_TUNING.md
├── STATIC_AUDIT.txt
├── VERSION.txt
└── README.md
```

---

**Platform:** Windows Subsystem for Linux  
**Language:** C++17 · OpenCV  
**Architecture:** L1 / L2 / L3 / VALID  
**Firefly version:** [motion_filter_firefly](../motion_filter_firefly/)
