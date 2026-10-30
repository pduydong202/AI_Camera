# Điều tra gap PTS và cảnh báo MPP — 29/10/2026

Đo trên Firefly/Optiplex và nguồn giả lập đang chạy, bằng chương trình riêng và bản sao dự án. **Giữ nguyên reconnect, thuật toán và cấu hình vận hành chính.**

## Gap trong video: đã khoanh vùng ở RTP → PTS

Ở nhóm clip có gap lớn nhất của bài 8.000 giây, **411 packet nén khớp nội dung giữa cả tám luồng**. Cùng một cặp frame có khoảng PTS từ **0,739 đến 0,960 giây** tùy kết nối. Đây là bất thường thời gian phát; chưa phải bằng chứng mất frame ở nhóm đã so sánh.

Đo hai kết nối đồng thời trong 300 giây, trước decoder và recorder:

| Chế độ RTSP | Frame RTP | Mất sequence quan sát được | Bước RTP lớn nhất | Bước PTS lớn nhất sau rtspsrc/depay |
|---|---:|---:|---:|---:|
| `buffer-mode=3` (auto) | 5.981 | 0 | 50 ms | **326,903 ms** |
| `buffer-mode=0` | 5.980 | 0 | 50 ms | **50 ms** |

Có 5.979 frame khớp payload giữa hai kết nối; chênh một frame ở biên bắt đầu/kết thúc. Tại điểm lỗi, timestamp RTP tăng đúng 4.500 tick/90 kHz = 50 ms, nhưng PTS do `rtspsrc` xuất ra tăng 326,903 ms, trước cả depay/parser. **Đã tái hiện lỗi ánh xạ đồng hồ trong đường nhận RTSP mode auto; không cần thuật toán, MPP, mux MP4 hoặc HTTP để tái hiện.** Thời gian buffer ra có những đợt dồn; phép đo này chưa phân biệt độ trễ ở publisher, mạng và lịch chạy trên máy nhận.

Bản thử **mode 0, 8 luồng / 300 giây** đạt khoảng **20,00–20,02 FPS/luồng**, gửi **160/160 event**. Kiểm tra toàn bộ **160 clip / 37.048 packet**: hash, danh tính và PTS/DTS đạt; khoảng PTS lớn nhất **50 ms**, không clip nào trên 100 ms. Giải mã **31 clip / 9.313 frame**, không lỗi. Bộ đối chứng mode 3 ở lượt tải này cũng không có gap trên 100 ms: lỗi có tính gián đoạn, không xảy ra ở mọi lượt 300 giây.

**Giới hạn:** đầu ra decoder MPP có một bước nhảy PTS trong khoảng khởi động ở cả hai mode (phép đo riêng: mode 0 khoảng 900 ms, mode 3 khoảng 876 ms). Trong bài 8 luồng, nhánh phân tích sau giây thứ 5 đều có bước PTS tối đa 50 ms. Hiện tượng khởi động này không xuất hiện ở nhánh video nén và cần tách khỏi gap trong clip.

## MPP: lỗi gọi hàm lúc đóng decoder

Tái hiện với pipeline `rtspsrc → depay → parse → mppvideodec → fakesink/appsink`, không có mã thuật toán dự án; có và không gửi EOS trước đóng đều gặp cảnh báo. Plugin cài trên Firefly là **`gstreamer1.0-rockchip1 1.14-4ubuntu2.2firefly6`**, thư viện MPP **`1.5.0-1`**.

Stack trace chỉ vào `libgstrockchipmpp.so+0xa960`. Mã máy tại lời gọi `0xa95c` nạp **giá trị frame handle** rồi gọi `mpp_frame_deinit`, trong khi [API MPP nhận địa chỉ handle](https://github.com/rockchip-linux/mpp/blob/develop/mpp/base/mpp_frame.c#L114). Kiểm tra trực tiếp xác nhận tham số nhận được tự nó là một MppFrame hợp lệ: caller đã truyền thiếu một mức địa chỉ. [Mã plugin upstream hiện dùng `&self->mpp_frame`](https://github.com/JeffyCN/mirrors/blob/gstreamer-rockchip/gst/rockchipmpp/gstmppdec.c#L302).

| Thử tối giản, cùng tám vòng đóng/mở | Lỗi deinit | Đối tượng MppFrame còn trong pool khi thoát |
|---|---:|---:|
| Plugin nguyên trạng | **8** | **8 × 224 byte** |
| Sửa hợp đồng tham số chỉ trong tiến trình chẩn đoán | **0** | Không còn cảnh báo đối tượng chưa trả |

Đây là đối tượng còn sót theo vòng đời decoder, không phải mỗi frame. Chưa thấy nó gây hỏng clip trong các bài đã kiểm tra. Worker dự án chạy ở tiến trình riêng và thoát khi reconnect, nên hệ điều hành thu hồi bộ nhớ tiến trình; điều đó giải thích tác động vận hành hiện còn nhỏ. Số 224 byte là kích thước đối tượng pool đo được, không phải ước lượng toàn bộ tài nguyên liên quan.

## Hướng xử lý

**Mode 0 là ứng viên cho bài 8.000 giây tiếp theo**, cần kiểm tra cả startup/reconnect và camera thực trước khi đổi mặc định. Lỗi MPP nên sửa trong bản build plugin riêng hoặc phiên bản plugin đã đối chiếu tương thích, kiểm tra đóng/mở và bộ nhớ trước triển khai. Bộ sửa tham số bằng preload vừa dùng chỉ phục vụ chứng minh nguyên nhân, không được cài vào môi trường vận hành.

Chứng cứ cục bộ: `motion_filter_firefly/build_deploy/pts_trial_20261029/gap_mpp_investigation_20261029/` (Git bỏ qua). Các bài đo đã kết thúc; thư viện hệ thống và mã/cấu hình chính không thay đổi.
