# Kiểm thử bản sửa PTS và HTTP — 29/10/2026

Thực hiện trên **bản thử riêng** ở Firefly RK3588 và Optiplex, dùng nguồn RTSP H.264 1080p/20 FPS, phân tích 854×480. Chỉ đưa thay đổi về mã chính sau khi build, self-test, chạy native và đối chiếu clip đạt yêu cầu. **Không thay thuật toán hoặc ngưỡng L1/L2/L3.**

## Kết quả

| Bài thử | Thời gian | Server nhận / sự kiện | FPS phân tích trung bình/luồng |
|---|---:|---:|---:|
| 8 luồng nguồn giả lập đang phát | 900 giây | **432/432** | **19,99–20,00** |
| 8 luồng có lỗi RTP timestamp và ngắt nhịp chủ động | 180 giây | **79/79** | 19,53–19,62 |
| Smoke test bản chốt, 8 luồng có lỗi chủ động | 60 giây | **27/27** | 19,05–19,68 |

Ở bài 900 giây: CPU toàn Firefly trung bình **53,48%**, nhiệt độ cao nhất **53,62°C**, tổng RSS worker và supervisor cao nhất **1,30 GiB**. CPU Optiplex trung bình khoảng **21,92%** theo mẫu một giây. Không worker nào restart hoặc bị buộc dừng trong các lượt đạt. Bài tạo lỗi chủ động làm gián đoạn nguồn nên không dùng FPS của bài đó để đánh giá tốc độ thuật toán.

Đã kiểm tra toàn bộ **538 clip / 137.244 packet**: hash khớp metadata, đủ clip so với ACK, không PTS trùng hoặc DTS lùi. Giải mã **83 clip mẫu / 25.034 frame**, không lỗi giải mã. Build và toàn bộ self-test trên Firefly đạt, gồm kiểm tra parity thuật toán, HTTP, recorder file và timeline.

## Thay đổi đã xác nhận

- **MP4 độ phân giải microgiây ở cả sender và receiver:** tái hiện được HTTP 422 trên bản cũ với hai PTS cách nhau khoảng 11 microgiây; bản sửa giữ đủ frame/payload, PTS/DTS trong sai số một microgiây.
- **Sửa PTS chung trước khi chia nhánh:** bài đối chứng bản cũ có 921 lần sửa PTS ở nhánh nén, lệch tối đa 673 ms. Các lượt đạt không còn sửa PTS tại hai consumer sau bước sửa chung. Nguồn có đồng hồ hỏng kéo dài được báo lỗi theo stall timeout để reconnect.
- **Receiver và chẩn đoán HTTP:** tăng backlog TCP từ 5 lên 32 với cấu hình mặc định, giữ giới hạn 16 upload đang xử lý; bổ sung chi tiết curl, vị trí lỗi và mức bộ đệm. Lượt chỉ sửa PTS trước đó vẫn có hai lỗi kết nối bị peer đóng; lượt thử lại không có lỗi gửi và bộ đếm listen overflow/drop không tăng. Chưa coi đó là bằng chứng mọi nguyên nhân reset mạng đã được loại bỏ.

## Giới hạn và bước tiếp theo

Vẫn có **321 clip có khoảng PTS trên 100 ms**, lớn nhất **1,067 giây**; khoảng tương ứng đã xuất hiện ở đầu vào nhánh nén. Bản sửa không che các khoảng trống nguồn hoặc khẳng định thời gian chụp/pre-post-roll luôn chính xác khi nguồn lỗi. Các bài dùng nhiều kết nối tới cùng một video, chưa đại diện camera độc lập, H.265 hoặc vận hành 24/7.

**Đã chạy tiếp bài 8.000 giây trên cả Firefly và receiver Optiplex: [kết quả có ngắt/nối lại nguồn](2026-10-29-8-streams-8000s-reconnect.md). Khi triển khai vẫn phải cập nhật cả hai phía.** Bản vận hành cũ trên hai máy được giữ nguyên; log, clip và chứng cứ thử nằm trong `~/AI_Camera_pts_trial_20261029/`, bản sao phân tích tại `motion_filter_firefly/build_deploy/pts_trial_20261029/` và không đưa lên Git.
