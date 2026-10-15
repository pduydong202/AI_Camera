# Dữ liệu kiểm thử

`recorder_h264.mp4` là clip tổng hợp nhỏ 64×48, 20 fps, 4 giây (80 access unit), tạo bằng FFmpeg để kiểm tra recorder. Không phải cảnh camera được đưa vào thuật toán phát hiện. Test so sánh compressed picture/PTS/DTS, hoàn tất MP4 và đường lỗi writer; đầu vào cố định ở đây giúp phát hiện hồi quy.

Các cảnh thuật toán trong `test_sparse_l3.cpp` và `test_algorithm_config.cpp` được sinh trong bộ nhớ. Bản tham chiếu ở `tests/reference` được giữ nguyên để đối chiếu; không dùng làm đường xử lý production. Tham số fixture chỉ thuộc test, không ghi đè cấu hình camera khi chạy ứng dụng.
