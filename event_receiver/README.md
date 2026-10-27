# Bộ nhận video sự kiện

Receiver thử nghiệm cho Optiplex Ubuntu 24.04, dùng Python standard library,
FFmpeg và GStreamer. Nhận trực tiếp fragmented MP4 từ Firefly qua HTTP, remux
không encode rồi lưu clip/metadata trên Optiplex. Chạy LAN/VPN trước; chưa tích hợp AI.

Khi triển khai/cập nhật, luôn chép `server.py` cùng `fmp4_timing.py` trong một
thư mục. Module này khôi phục default duration/size bị bỏ sót trong fragment từ
GStreamer 1.20, rồi bộ nhận kiểm tra PTS duy nhất và DTS tăng nghiêm ngặt trước
khi báo READY. Xem [kết quả sửa timestamp](../motion_filter_firefly/docs/TIMESTAMP_REPAIR.md).

Hướng dẫn cài đặt, cấu hình Firefly, kiểm thử và systemd:
[NETWORK_DELIVERY.md](../motion_filter_firefly/docs/NETWORK_DELIVERY.md).

Nếu gặp `422` hoặc `Buffer has no PTS`, xem
[hướng dẫn sửa và kiểm tra lại mẫu lỗi](../motion_filter_firefly/docs/TROUBLESHOOT_HTTP_422.md).

```bash
sudo apt-get install -y python3 ffmpeg gstreamer1.0-tools \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
export VIDEO_SERVER_TOKEN='<token-giong-Firefly>'
python3 server.py --host 0.0.0.0 --output ./received
```
