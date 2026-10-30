# Thuật toán và luồng xử lý

## Nguồn và thời gian

RTSP → depay/parser → tee: một nhánh lấy access unit nén để ghi clip, một nhánh qua MPP ra NV12. Ảnh NV12 được RGA thu nhỏ, CPU đọc Y plane trực tiếp; không đổi sang BGR trong đường xử lý chính. Tỷ lệ khung hình được giữ, kích thước chẵn để phù hợp NV12.

RTSP mặc định dùng `buffer-mode=0` để giữ nhịp RTP của nguồn. Khi khởi động hoặc
reconnect, gate trước tee chờ access unit random access (H.264 IDR hoặc H.265
IRAP), ghi số AU bỏ qua trong `[SOURCE_START]`, rồi truyền mọi AU còn lại.
Header hoặc P-frame chưa giải mã được trước keyframe không vào decoder/recorder;
không bù thời gian bằng cách sửa nhịp các frame hợp lệ. Warmup bắt đầu từ ảnh
giải mã hợp lệ đầu tiên; các ngưỡng và bước L1/L2/L3 không thay đổi.

Depayloader RTSP thương lượng `alignment=au` trước parser. Khi nhận EOS do
ngắt nguồn, guard chuyển EOS xuống parser mà không cho depayloader xả các
slice đang ghép thành một hình chưa đầy đủ. AU đã hoàn chỉnh giữ nguyên
payload và timestamp; phần đuôi chưa được xác nhận hoàn tất có thể bị bỏ.
Nguồn thiếu RTP marker cũng chịu chính sách thận trọng này. Log
`[SOURCE_EOS] complete_aus_only=1` xác nhận đường EOS, không phải số frame bỏ.
Guard không áp dụng cho file đầu vào.

Giới hạn FPS live dùng thời gian monotonic và credit burst có giới hạn; không thay PTS. Appsink ảnh có bộ đệm hữu hạn, có thể bỏ ảnh cũ khi quá tải. Sau keyframe khởi động, nhánh nén giữ các AU hoàn chỉnh theo thứ tự và có thể tạo backpressure khi ghi chậm. Chỉ queue ảnh decoded được phép drop theo YAML. Queue nén không dùng leaky.

Với RTSP, PTS được kiểm tra một lần sau parser và trước tee để nhánh phân tích và nhánh ghi clip nhận cùng quyết định sửa. PTS hợp lệ tăng ít nhất một microgiây được giữ nguyên; timestamp thiếu/trùng/lùi hoặc sát nhau dưới một microgiây được tăng bằng bước nhỏ nhất một microgiây, có log `[SOURCE_TIME]`. Không cộng nguyên một frame danh nghĩa cho từng lỗi vì có thể tạo chuỗi catch-up kéo dài. Đây là sửa thứ tự timeline, không khôi phục thời gian chụp thật khi timestamp nguồn đã hỏng; các khoảng trống nguồn hợp lệ vẫn được giữ.

MP4 trên Firefly và bước remux trên receiver dùng `trak-timescale=1000000`, độ phân giải một microgiây, để tránh hai PTS gần nhau bị làm tròn thành trùng nhau. Không đổi payload nén hoặc ép video về FPS cố định. Chẩn đoán `[PTS]` ở hai nhánh và cơ chế phòng vệ sau decoder vẫn được giữ; `[SEND DIAG]`/`[SEND DETAIL]` bổ sung vị trí lỗi và mức bộ đệm khi writer/upload lỗi.

Trường `raw` trong `[PTS]` là timestamp tại đầu vào của từng consumer, sau bước sửa chung. Dùng `[SOURCE_TIME]` để xem timestamp nguồn trước/sau sửa; số lần sửa ở hai consumer bằng 0 không có nghĩa nguồn không từng phát timestamp lỗi.

Nếu timestamp nguồn liên tục không khôi phục trong `rtsp_stall_timeout_sec`, nguồn được báo lỗi để supervisor reconnect. Không để một nguồn có đồng hồ hỏng vĩnh viễn tiếp tục chạy với timeline tăng quá chậm và không bao giờ qua warmup.

## L1: vùng ứng viên

MOG2 nhận ảnh xám. YAML khai báo history, variance threshold và learning rate; mặc định 500, 16, -1 (tự động). Tắt shadow detection để mask luôn nhị phân 0/255. OPEN với kernel vuông mặc định **3×3** loại các cấu trúc nhỏ/đứt. Không có thao tác bỏ kernel đã kiểm chứng. Bản thân kernel là phép hình thái cục bộ, không tương đương một ngưỡng diện tích vật thể.

Tính connected components đúng một lần, mặc định liên thông 8 hướng. L2, L3 và quyết định cuối dùng chung nhãn thành phần này.

## L2: bằng chứng hoạt động lặp

L2 gom mật độ foreground vào lưới, kích thước ô đi theo TAWL W. Theo dõi occupancy và re-entry trong cửa sổ hữu hạn; re-entry là tăng sau một lần giảm, trạng thái chờ cũng hết hạn theo cùng history. History mặc định đi theo MOG2 (500 frame); có thể khai báo độc lập bằng `l2_history_frames` >0.

Tích occupancy×re-entry được giữ trong [0,1], lượng tử hóa 8 bit rồi Otsu trên **toàn lưới, kể cả ô 0**. Không kéo min/max để khuếch đại bằng chứng yếu. Giãn vùng lặp theo neighborhood mặc định 3×3 ô, phóng lên kích thước phân tích bằng nearest-neighbor.

Một thành phần hiện tại chỉ được L2 giải thích là lặp nếu **mọi pixel** của nó nằm trong vùng lặp lịch sử. Một phần đi ra ngoài thì giữ nguyên cả thành phần như chuyển động mới. L2 không xóa trực tiếp mask L1, không có tracker hay phân loại xe/người.

Mật độ dao động bên trong cùng ô có thể không tạo re-entry đủ mạnh; lượng tử hóa 8 bit có thể làm mất bằng chứng nhỏ. Chỉ đổi sang 16 bit đã làm mất frame xe xác nhận trong phép replay, nên bị loại. Không xem test L2 hiện tại là bằng chứng đã lọc được mọi cảnh cây rung.

## L3: thời gian, chiều rộng, vị trí

TA = ceil(`tawl_temporal_fps_ratio` × fps phân tích danh nghĩa), tối thiểu 1 frame. W = ceil(`tawl_width_frame_ratio` × chiều rộng ảnh phân tích), tối thiểu 1 pixel. Với 854×480 và nguồn/mục tiêu 20 fps, TA=4 frame, W=43 pixel.

Tuổi được kế thừa ở mức thành phần qua neighborhood vị trí; thành phần đủ tuổi tạo seed đối tượng. Run ngang **dài hơn W** cũng tạo seed. Giãn seed tạo vùng bảo vệ lân cận. Pixel foreground ngắn, hẹp và ngoài vùng bảo vệ là bằng chứng mưa/nhiễu.

Sparse L3 chỉ tính trong bounding rectangle chứa foreground hiện tại, cộng lân cận cần thiết khi đọc tuổi. Khi không foreground, xóa tuổi như đường toàn ảnh. Đây là giảm phần tính toán, không thay ngưỡng hay bỏ mask: bài kiểm thử đối chiếu mọi trường FrameResult, bốn mask và tín hiệu sự kiện với bản tham chiếu qua 1.300 frame.

## VALID và sự kiện

Bỏ một thành phần khi L2 giải thích toàn bộ nó **hoặc** L3 giải thích toàn bộ nó. Phần còn lại được giữ nguyên cả thành phần. VALID ratio là số pixel giữ lại / tổng pixel ảnh phân tích.

Sau warmup theo PTS, `hasValidMotion` đúng khi có VALID và ratio ≥ `event_min_valid_ratio`. EventManager mở sự kiện, kéo dài khi còn motion, đóng sau khoảng yên lặng `event_post_sec`; kết thúc tiến trình cũng đóng sự kiện đang mở. Clip bắt đầu từ keyframe thích hợp trước mốc pre-roll nếu có trong ring, giữ payload nén gốc và rebase PTS/DTS về timeline clip.

Các cửa sổ TA/L2 tính bằng **frame đã phân tích**, không theo số frame nguồn đã giải mã. Khi phân tích bị tụt FPS/bỏ frame, khoảng thời gian vật lý tương ứng có thể đổi; test tương đương không chứng minh độ chính xác không đổi dưới quá tải.
