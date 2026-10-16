# Audit

Bản này không thêm threshold thuật toán mới.

Giữ nguyên các lựa chọn đã chấp nhận:
- morphology kernel `3x3`
- TAWL temporal ratio / width ratio
- `event_min_valid_ratio`
- analysis resolution/FPS, warmup, pre/post là policy cấu hình

Ba fix hiện tại:
1. Component TA dùng maximum inherited age.
2. `reentryArmed` hết hạn theo cùng `l2HistoryFrames`.
3. Otsu dùng trực tiếp repeat evidence `[0,1]` và toàn bộ grid, không min-max positive-only.

Không thêm tracker, IoU, centroid/distance threshold, optical flow, SHIFT, area threshold hay score mới.


## VALID mask
`full_VALID_motion.mp4` and `motion_XXXXXX_VALID.mp4` show the full connected components already counted by the unchanged event logic as valid motion. Adding these files does not change detection or event decisions.
