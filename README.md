# 🎥 AI_Camera

**Motion Detection & Event Recording System**

Hệ thống phát hiện chuyển động và trích xuất video sự kiện từ camera, phát triển bằng **C++17 và OpenCV**, tập trung vào tối ưu CPU và xử lý nhiều luồng video.

### 🛠️ Tech Stack

![C++](https://img.shields.io/badge/C++-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![OpenCV](https://img.shields.io/badge/OpenCV-5C3EE8?style=for-the-badge&logo=opencv&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white)
![GStreamer](https://img.shields.io/badge/GStreamer-FF3131?style=for-the-badge&logo=gstreamer&logoColor=white)
![Bash](https://img.shields.io/badge/Bash-4EAA25?style=for-the-badge&logo=gnubash&logoColor=white)
![Ubuntu](https://img.shields.io/badge/Ubuntu-E95420?style=for-the-badge&logo=ubuntu&logoColor=white)
![Firefly RK3588](https://img.shields.io/badge/Firefly-RK3588-F15A24?style=for-the-badge&logo=arm&logoColor=white)
### 📂 Project Structure

| Module | Description | Platform |
|---|---|---|
| 🖥️ [motion_filter_wsl](motion_filter_wsl/) | Kiểm thử thuật toán với video offline | Windows / WSL |
| 🔥 [motion_filter_firefly](motion_filter_firefly/) | Xử lý RTSP, tối ưu CPU với MPP/RGA | Firefly RK3588 |

### ⚙️ Processing Pipeline

```text
       Video Input
            │
            ▼
    L1 — Motion Detection
            │
            ▼
    L2 — Repetitive Motion
            │
            ▼
    L3 — Rain / Noise Filter
            │
            ▼
     VALID Motion Check
            │
            ▼
     Event Video Clips
```

### ✨ Key Features

- **L1/L2/L3:** Phát hiện chuyển động và giảm nhiễu môi trường.
- **Event Recording:** Tự động ghi video khi phát hiện sự kiện.
- **Hardware Acceleration:** Sử dụng Rockchip MPP và RGA trên Firefly.
- **Multi-Camera:** Hỗ trợ xử lý nhiều nguồn RTSP trên Firefly.
- **Configurable:** Quản lý tham số thuật toán bằng YAML.

### 📖 Documentation

Hướng dẫn cài đặt, chạy chương trình và tài liệu kỹ thuật:

- [Firefly RK3588 — README](motion_filter_firefly/README.md)
- [Windows / WSL — README](motion_filter_wsl/README.md)

---

**Language:** C++17 · Bash  
**Platforms:** Firefly RK3588 · Windows Subsystem for Linux
