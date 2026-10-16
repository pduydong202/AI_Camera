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

**Yêu cầu:** WSL/Linux, C++17, CMake, OpenCV và Bash.

Clone repository:

```bash
git clone https://github.com/pduydong202/AI_Camera.git
cd AI_Camera/motion_filter_wsl
```

Kiểm tra môi trường và biên dịch:

```bash
bash check_wsl.sh
bash build_wsl.sh
```

Chạy với video đầu vào:

```bash
./build/motion_filter "/path/to/video.mp4"
```

Thay đường dẫn ví dụ bằng đường dẫn video thực tế.

## 4. Cấu hình

| File | Độ phân giải phân tích tối đa |
|---|---|
| `policy.yaml` | 854×480 |
| `policy_360p.yaml` | 640×360 |
| `policy_720p.yaml` | 1280×720 |

Các cấu hình cho phép thử nghiệm thuật toán ở nhiều độ phân giải mà không cần sửa mã nguồn.

## 5. Kết quả đầu ra

Hệ thống tạo clip sự kiện và các video mask để kiểm tra từng lớp.

**Full-length masks:**

```text
masks_full/
├── full_L1_foreground.mp4
├── full_L2_repeat_evidence.mp4
├── full_L3_rain_noise_evidence.mp4
└── full_VALID_motion.mp4
```

**Event-aligned masks:**

```text
masks/
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
