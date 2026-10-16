# Motion Filter - WSL

Bản WSL test theo kiến trúc L1/L2/L3/VALID hiện tại. Bản này chỉ sửa 3 lỗi đã thống nhất, không thêm feature mới.

```text
L1 = MOG2 foreground + opening 3x3
L2 = repetitive-motion evidence từ grid history + component containment
L3 = TAWL rain/noise evidence
Event = valid connected-component area ratio
```

## 3 sửa đổi

### 1. Component TA lấy maximum age

Một connected component hiện tại nhận **maximum inherited age** từ các pixel 8-neighbor của frame trước. Không còn lỗi pixel duyệt sau ghi đè tuổi lớn hơn của pixel trước.

### 2. `reentryArmed` có thời hạn

Cell sau khi activity giảm có thể chờ một lần rise để tạo re-entry, nhưng trạng thái armed chỉ sống trong đúng cùng cửa sổ lịch sử L2. Khi ra khỏi cửa sổ đó thì tự hết hạn. Không thêm tham số tuning mới.

### 3. Otsu của L2 không min-max các giá trị dương

`repeatEvidence` vốn đã nằm trong `[0,1]`. Code scale trực tiếp `[0,1] -> [0,255]` và cho Otsu nhìn toàn bộ grid, kể cả zero cells. Evidence rất nhỏ không còn bị kéo giãn thành mạnh chỉ vì nó là giá trị dương lớn nhất trong frame.

## Giữ nguyên

- kernel `3x3`
- TAWL TA/W/Location
- `event_min_valid_ratio: 0.005`
- L2 component containment
- component chưa được L2/L3 giải thích 100% thì tính toàn component vào valid area
- mặc định `854x480 @ 20 FPS`, đổi được trong YAML

## Build/run

```bash
chmod +x check_wsl.sh build_wsl.sh
./check_wsl.sh
./build_wsl.sh
./build/motion_filter "/mnt/d/camera_AI_thuat_toan/video_test/8.mp4"
```

## Debug masks

```text
masks_full/full_L1_foreground.mp4
masks_full/full_L2_repeat_evidence.mp4
masks_full/full_L3_rain_noise_evidence.mp4
```

Event-aligned masks vẫn nằm trong `masks/`.


## VALID mask
`full_VALID_motion.mp4` and `motion_XXXXXX_VALID.mp4` show the full connected components already counted by the unchanged event logic as valid motion. Adding these files does not change detection or event decisions.
