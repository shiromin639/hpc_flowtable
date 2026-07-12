
# FlowCore Current Documentation

Bộ tài liệu này mô tả trạng thái hiện tại của mã nguồn FlowCore DPDK. Các file cũ đã được thay bằng tài liệu mới để tránh nhầm giữa thiết kế cũ và code đang chạy.

- `current_architecture.md`: kiến trúc runtime, project structure, pipeline, thread/port/ring model và flow lifecycle.
- `current_optimization.md`: các kỹ thuật tối ưu chính đang dùng, lý do chọn, tradeoff và alternative.
- `defense_guide_current.md`: hướng dẫn bảo vệ chi tiết, bao gồm cấu trúc project, struct/field, câu hỏi phản biện, kịch bản lỗi, hướng ôn tập DPDK/C/OS/kiến trúc máy tính.
- `flowcore_report_vietnamese.tex`: bản nháp báo cáo LaTeX tiếng Việt.

Nguồn yêu cầu gốc là `MiniProject_HPC_Flowtable.docx`; file `docs/docx_content.txt` chỉ là bản trích nội dung để tham khảo nhanh. Khi có mâu thuẫn giữa tài liệu cũ và source code, ưu tiên source code hiện tại.
