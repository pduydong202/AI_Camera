# Plugin Rockchip riêng cho dự án

`bash build.sh` build cả ứng dụng và plugin GStreamer Rockchip vào `build_gst/`.
Không cần sudo, pip hoặc cài Meson toàn hệ thống. Lần đầu cần mạng để tải hai
archive có commit/version và SHA-256 cố định trong `tools/rockchip_plugin.lock.json`.
Các lần sau dùng cache đã kiểm tra hash. Cần Python 3.10+, Ninja, trình biên dịch C,
pkg-config và header GStreamer/MPP có sẵn trên Firefly.

Plugin hệ thống của thiết bị thử gọi sai `mpp_frame_deinit`: truyền handle thay
vì địa chỉ handle. Source upstream được chọn đã sửa lỗi này. Không vá binary và
không ghi vào `/usr/lib`; giữ nguyên thư viện MPP/RGA của thiết bị.

```bash
bash build.sh
bash check.sh
bash run.sh --replicas 8 --seconds 900
```

`run.sh` đặt `GST_PLUGIN_PATH_1_0` và registry riêng cho tiến trình này cùng các
worker, đồng thời xác nhận `mppvideodec` được lấy đúng từ plugin riêng. Lệnh chạy
thật với binary mặc định sẽ dừng nếu chưa có plugin. `--dry-run` vẫn chỉ kiểm tra
cấu hình và không cần kết nối camera. Thông tin plugin thực tế được lưu trong
log mỗi bài chạy; hash build nằm ở `build_gst/manifest.json`.

Build riêng khi cần:

```bash
python3 tools/build_rockchip_plugin.py
```

Khi gọi binary hoặc công cụ GStreamer trực tiếp, cần đặt hai biến môi trường
như launcher; chạy `./build/motion_filter` một mình không tự chọn plugin riêng:

```bash
export GST_PLUGIN_PATH_1_0="$PWD/build_gst/prefix/lib/gstreamer-1.0${GST_PLUGIN_PATH_1_0:+:$GST_PLUGIN_PATH_1_0}"
export GST_REGISTRY_1_0="$PWD/build_gst/registry.bin"
gst-inspect-1.0 mppvideodec
```

`MOTION_GST_PLUGIN_DIR` cho phép launcher dùng một thư mục plugin khác khi thử
đối chứng. `MOTION_FILTER_BIN` dành cho binary thử/self-test. Không dùng hai biến
override để bỏ qua quy trình xác nhận plugin khi vận hành.

Source được pin tại commit `66be2501caa40e4abe93c889a7364af2497ea1d7` của
[gstreamer-rockchip](https://github.com/JeffyCN/mirrors/tree/66be2501caa40e4abe93c889a7364af2497ea1d7),
có giấy phép LGPL trong archive gốc. Plugin ở prefix riêng không đổi cách các
ứng dụng khác trên máy sử dụng GStreamer.
