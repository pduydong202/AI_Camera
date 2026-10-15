# Thuật toán và luồng xử lý

## Nguồn và thời gian

RTSP → depay/parser → tee: một nhánh lấy access unit nén để ghi clip, một nhánh qua MPP ra NV12. Ảnh NV12 được RGA thu nhỏ, CPU đọc Y plane trực tiếp; không đổi sang BGR trong đường xử lý chính. Tỷ lệ khung hình được giữ, kích thước chẵn để phù hợp NV12.

Giới hạn FPS live dùng thời gian monotonic và credit burst có giới hạn; không thay PTS. Appsink ảnh có bộ đệm hữu hạn, có thể bỏ ảnh cũ khi quá tải. Nhánh nén không chủ động bỏ access unit để giữ chuỗi giải mã, nhưng có thể tạo backpressure khi ghi chậm. Chỉ queue ảnh decoded được phép drop theo YAML. Queue nén phải giữ thứ tự và không dùng leaky.

Normalizer hiện tại giữ PTS dùng trong ứng dụng tăng dần khi PTS gốc thiếu/trùng/lùi. Chẩn đoán ghi riêng raw/used, lý do sửa và độ lệch cho hai nhánh. Quy tắc này chưa được thiết kế lại trong bản gom mã; có thể tạo chuỗi sửa kéo dài dưới tải cao.

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
