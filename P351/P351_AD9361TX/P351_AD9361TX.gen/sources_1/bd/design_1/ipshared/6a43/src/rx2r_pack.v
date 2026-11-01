// ============================================================================
// rx2r_pack.v —— TX-DAC / RX-ADC 双通道 I/Q 监视打包器
//
// RX 输入（data_clk 域）：ad936x_phy 解出的 ch1(I0,Q0)+ch2(I1,Q1)。
// TX 输入（tx_sample_clk 域）：真正送往 LVDS PHY 的 DAC I/Q。TX 数据通过事件
// toggle + 三级 bundled-data 采样安全搬运到 data_clk 域，不新增异步 FIFO/BRAM。
// 打包：每组 ch1+ch2 → 2 个 32bit AXI-Stream 字，每字 = 一个复样本
//       {Q[15:0], I[15:0]}（12bit 符号扩展，I 低 16 位、Q 高 16 位），ch1 字、ch2 字相邻。
// 采集门控：PS 写 N，再写 CTRL 的 capture_en(bit0) 上升沿触发一次捕获，到 N 字后 done。
//           （单次捕获语义：捕获总是只做一次，PS 须清 CTRL 再置才能再次触发。
//             原 one_shot bit1 保留为兼容位，不再参与 FSM。）
// 时间戳：data_clk 域 64bit 自由计数器，捕获起点锁存供 PS 读。
//
// 2026-08-04 修复记录（Workflow 审查后，含仿真验证修正）：
//   P0#1 tready 门控：字在 (tvalid_r && m_axis_tready) 时才提交（wcnt++/推进），
//         tready 低时保持当前字不丢；装载 ch1 时同时快照整组 (i0,q0,i1,q1)，
//         背压延迟期间 ch2 用快照，不受新组数据覆盖。提交/装载流水线化，
//         吞吐保持 1 字/拍（40M 字/s）。
//   P0#2 AXI-Lite 地址解码：OPT_MEM_ADDR_BITS 2→3，解码 addr[4:2]，修复 0x10/0x14
//         （TS_LO/TS_HI）被 2-bit 截断别名到 CTRL/N 的 bug；写地址锁存 AWADDR，
//         AW 与 W 不同拍到达时也能正确落寄存器。
//   P1#3 采集触发改为 capture_en 上升沿，done 后回 ST_IDLE 不自动重启。
//   P2#4 N 内部强制偶数（N_even = N & ~1），N=0 视为空捕获（done 立即置位）。
//   P2#6 overflow_r 进入 ST_IDLE 时清零；ST_CAPT 中 (tvalid_r&&!m_axis_tready) 置位
//         （背压指示，非丢数据 —— 门控后不丢字）。
//   P3#7 时间戳经 2-FF 同步器跨到 100MHz S_AXI_ACLK 域。
//   P3#8 删除死代码（n_d1..n_d6、reg_read）。
//   [仿真修正] 状态机用 is_ch2（挂起的字是否 ch2）明确语义；tlast 用 wcnt+2 判断
//         （装载 ch2 为第 wcnt+2 字）、done 用 wcnt+1 判断（提交 ch2 后字数）；
//         done 拍不装载多余字。
//
// AXI-Lite 寄存器（基址 0x4xxxxxxx，64K）：
//   reg0 (0x00) RW: bit0 capture_en（上升沿触发捕获）, bit1 one_shot(兼容保留)
//   reg1 (0x04) RW: N —— 每次捕获的 32bit 字数（内部强制偶数；须偶数以满足 ch1/ch2 成对）
//   reg2 (0x08) RO: STATUS —— bit0 done, bit1 backpressure(背压曾发生), bit2 armed(ST_CAPT)
//   reg3 (0x0C) RO: 已写入字数（源端寄存Gray码、2级同步后解码）。
//         2026-08-06 修复：wcnt 在 PS 清 capture_en 前保持（不再回 IDLE 即清零），
//         PS 在 done 后读 CNT 可得本次实际写入字数。PS 清 CTRL 后再触发时清零。
//   reg4 (0x10) RO: 捕获起点时间戳低 32bit（2-FF 同步，捕获停止后读取）
//   reg5 (0x14) RO: 捕获起点时间戳高 32bit
//   reg6 (0x18) RW: SOURCE —— bit0 0=RX ADC，1=TX DAC（仅在空闲时切换）
// STATUS 扩展：bit3 当前采集源，bit4 自复位后至少见过一个 TX DAC 样本，
//               bit5 自复位后至少见过一个 RX ADC 有效样本。
// ============================================================================
`timescale 1ns / 1ps

module rx2r_pack #(
    parameter integer TX_ONLY = 0,
    // P351 1R1T: DMA只送真实逻辑槽0，不让空槽占一半总线带宽。
    // 0保留双槽历史格式，1每复样本一个32bit字。PS负责兼容UDP零槽。
    parameter integer SINGLE_SLOT = 1,
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
)(
    // AXI-Lite 从口（100MHz S_AXI_ACLK 域）
    input  wire                         S_AXI_ACLK,
    input  wire                         S_AXI_ARESETN,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
    input  wire                         S_AXI_AWVALID,
    output wire                         S_AXI_AWREADY,
    input  wire [C_S_AXI_DATA_WIDTH-1:0] S_AXI_WDATA,
    input  wire [C_S_AXI_DATA_WIDTH/8-1:0] S_AXI_WSTRB,
    input  wire                         S_AXI_WVALID,
    output wire                         S_AXI_WREADY,
    output wire [1:0]                   S_AXI_BRESP,
    output wire                         S_AXI_BVALID,
    input  wire                         S_AXI_BREADY,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
    input  wire                         S_AXI_ARVALID,
    output wire                         S_AXI_ARREADY,
    output wire [C_S_AXI_DATA_WIDTH-1:0] S_AXI_RDATA,
    output wire [1:0]                   S_AXI_RRESP,
    output wire                         S_AXI_RVALID,
    input  wire                         S_AXI_RREADY,
    // ADC 数据（data_clk 域）
    input  wire                         data_clk,
    input  wire                         rst,          // data_clk 域高有效复位
    input  wire                         adc_valid,    // 每组有效(2 时钟 1 拍)
    input  wire [11:0]                  adc_data_i0,  // ch1 I (RX1A)
    input  wire [11:0]                  adc_data_q0,  // ch1 Q
    input  wire [11:0]                  adc_data_i1,  // ch2 I (RX2A)
    input  wire [11:0]                  adc_data_q1,  // ch2 Q
    // TX DAC 数据（tx_sample_clk 域；与 data_clk 同源，通常为 61.44/245.76 MHz）
    input  wire                         tx_sample_clk,
    input  wire                         tx_dac_valid,
    input  wire [11:0]                  tx_dac_data_i0,
    input  wire [11:0]                  tx_dac_data_q0,
    input  wire [11:0]                  tx_dac_data_i1,
    input  wire [11:0]                  tx_dac_data_q1,
    // AXI-Stream 主口（data_clk 域，接 axis_data_fifo 写侧）
    output wire [31:0]                  m_axis_tdata,
    output wire                         m_axis_tvalid,
    input  wire                         m_axis_tready,
    output wire                         m_axis_tlast
);

    // ======================== AXI-Lite 寄存器读写 ========================
    localparam ADDR_LSB = (C_S_AXI_DATA_WIDTH/32) + 1;   // =2 (字节偏移)
    localparam OPT_MEM_ADDR_BITS = 4;   // V7: addr[5:2]，保留旧0..6，接收控制8..15

    reg [C_S_AXI_DATA_WIDTH-1:0] slv_reg0, slv_reg1, slv_reg2, slv_reg3, slv_reg4, slv_reg5;
    reg [C_S_AXI_DATA_WIDTH-1:0] slv_reg6;
    reg [31:0] rx_ctrl=0,rx_rate=279620267,rx_sync_word=32'hfe6b2840,rx_frame_bits=32'h80011000;
    reg [2:0] rx_counter_select=0;
    reg [23:0] rx_recip=251658;reg [31:0] rx_aux=0;
    reg rx_timing_request=0;
    wire [31:0] rx_dsp_status,rx_dsp_counter,rx_fifo_data;
    wire rx_fifo_empty;

    // 写握手寄存器（支持 AW/W 同拍或分拍到达）
    reg axi_awready, axi_wready, axi_bvalid;
    reg aw_h, w_h;                              // AW/W 已接收标志
    reg [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr;   // P0#2: 锁存 AWADDR
    reg [C_S_AXI_DATA_WIDTH-1:0] axi_wdata;    // 锁存 WDATA
    reg [C_S_AXI_DATA_WIDTH/8-1:0] axi_wstrb;  // 锁存 WSTRB
    reg axi_arready, axi_rvalid;
    reg [C_S_AXI_DATA_WIDTH-1:0] axi_rdata;
    wire [1:0] axi_bresp = 2'b00, axi_rresp = 2'b00; // AXI OKAY

    assign S_AXI_AWREADY = axi_awready;
    assign S_AXI_WREADY  = axi_wready;
    assign S_AXI_BRESP   = axi_bresp;
    assign S_AXI_BVALID  = axi_bvalid;
    assign S_AXI_ARREADY = axi_arready;
    assign S_AXI_RDATA   = axi_rdata;
    assign S_AXI_RRESP   = axi_rresp;
    assign S_AXI_RVALID  = axi_rvalid;

    // 写通道握手：AW 与 W 各自独立接受（同拍或分拍），都到齐后提交写寄存器并置 BVALID。
    integer byte_index;
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_awready <= 1'b0; axi_wready <= 1'b0; axi_bvalid <= 1'b0;
            aw_h <= 1'b0; w_h <= 1'b0;
            axi_awaddr <= 6'd0; axi_wdata <= 32'd0; axi_wstrb <= 4'd0;
            slv_reg0 <= 32'd0; slv_reg1 <= 32'd1024; slv_reg6 <= 32'd1;
            rx_ctrl<=0;rx_rate<=279620267;rx_sync_word<=32'hfe6b2840;rx_frame_bits<=32'h80011000;rx_counter_select<=0;
            rx_recip<=251658;rx_aux<=0;
            rx_timing_request<=0;
        end else begin
            axi_awready <= 1'b0; axi_wready <= 1'b0;
            if (axi_bvalid && S_AXI_BREADY) axi_bvalid <= 1'b0;
            if (!axi_bvalid) begin
                if (S_AXI_AWVALID && !aw_h) begin
                    axi_awready <= 1'b1;
                    axi_awaddr  <= S_AXI_AWADDR;
                    aw_h <= 1'b1;
                end
                if (S_AXI_WVALID && !w_h) begin
                    axi_wready <= 1'b1;
                    axi_wdata  <= S_AXI_WDATA;
                    axi_wstrb  <= S_AXI_WSTRB;
                    w_h <= 1'b1;
                end
                if (aw_h && w_h) begin
                    // 提交：AW 与 W 都到齐 → 写寄存器（用锁存的地址/数据）
                    case (axi_awaddr[ADDR_LSB+:OPT_MEM_ADDR_BITS])
                        4'h0: begin
                            for (byte_index = 0; byte_index < C_S_AXI_DATA_WIDTH/8; byte_index = byte_index+1)
                                if (axi_wstrb[byte_index])
                                    slv_reg0[byte_index*8 +: 8] <= axi_wdata[byte_index*8 +: 8];
                        end
                        4'h1: slv_reg1 <= axi_wdata;
                        4'h6: begin
                            // 仅 bit0 有效；默认 TX DAC，避免 RX 链路异常阻塞监视。
                            if (axi_wstrb[0]) slv_reg6[0] <= TX_ONLY ? 1'b1 : axi_wdata[0];
                            slv_reg6[31:1] <= 31'd0;
                        end
                        // 完整字写；仅停止时更改配置，PS检查状态确认停止后再提交。
                        4'h8: if(axi_wstrb==4'hf) rx_ctrl<=axi_wdata & 127;
                        4'h9: if(axi_wstrb==4'hf && !rx_ctrl[0]) rx_rate<=axi_wdata;
                        4'ha: if(axi_wstrb==4'hf && !rx_ctrl[0]) rx_sync_word<=axi_wdata;
                        4'hb: if(axi_wstrb==4'hf && !rx_ctrl[0]) rx_frame_bits<=axi_wdata;
                        4'he: if(axi_wstrb==4'hf) begin
                            if(axi_wdata[31] && !rx_ctrl[0]) begin rx_recip<=axi_wdata[23:0];rx_aux<=axi_wdata;end
                            else if(!axi_wdata[31]) begin
                                rx_counter_select<=axi_wdata[2:0];rx_aux<=axi_wdata;
                                if(axi_wdata[2:0]==4)rx_timing_request<=~rx_timing_request;
                            end
                        end
                        default: ;
                    endcase
                    axi_bvalid <= 1'b1;
                    aw_h <= 1'b0; w_h <= 1'b0;
                end
            end
        end
    end

    // 读握手 + 读寄存器（组合地址解码，P0#2 后 addr[4:2] 全覆盖 6 个寄存器）
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_arready <= 1'b0; axi_rvalid <= 1'b0; axi_rdata <= 32'd0;
        end else begin
            if (!axi_rvalid && S_AXI_ARVALID) begin
                axi_arready <= 1'b1;
                axi_rvalid  <= 1'b1;
                case (S_AXI_ARADDR[ADDR_LSB+:OPT_MEM_ADDR_BITS])
                    4'h0: axi_rdata <= slv_reg0;
                    4'h1: axi_rdata <= slv_reg1;
                    4'h2: axi_rdata <= slv_reg2;
                    4'h3: axi_rdata <= slv_reg3;
                    4'h4: axi_rdata <= slv_reg4;
                    4'h5: axi_rdata <= slv_reg5;
                    4'h6: axi_rdata <= slv_reg6;
                    4'h8: axi_rdata <= rx_ctrl;
                    4'h9: axi_rdata <= rx_rate;
                    4'ha: axi_rdata <= rx_sync_word;
                    4'hb: axi_rdata <= rx_frame_bits;
                    4'hc: axi_rdata <= rx_dsp_status;
                    4'hd: axi_rdata <= rx_fifo_empty?32'd0:rx_fifo_data;
                    4'he: axi_rdata <= rx_aux;
                    4'hf: axi_rdata <= rx_dsp_counter;
                    default: axi_rdata <= 32'hDEADBEEF;
                endcase
            end else begin
                axi_arready <= 1'b0;
                if (axi_rvalid && S_AXI_RREADY) axi_rvalid <= 1'b0;
            end
        end
    end

    // ---- 12bit 符号扩展 ----
    function [15:0] sext;
        input [11:0] v;
        sext = {{4{v[11]}}, v};
    endfunction

    // ======================== 采集/打包（data_clk 域） ========================
    // ---- CDC：PS(100MHz) 的控制位 → data_clk 域 ----
    // capture_en 三级同步（ce_d1->ce_d2->ce_d3），ce_d2/ce_d3 检测上升沿。
    // N两级同步；PS仅在停止时改N，并先写N再写CTRL，第三拍捕获边沿时锁存。
    (* ASYNC_REG = "TRUE" *) reg ce_d1, ce_d2, ce_d3;
    (* ASYNC_REG = "TRUE" *) reg source_d1, source_d2;
    (* ASYNC_REG = "TRUE" *) reg [31:0] N_meta, N_sync;
    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            ce_d1 <= 0; ce_d2 <= 0; ce_d3 <= 0;
            source_d1 <= 1'b1; source_d2 <= 1'b1;
            N_meta <= 32'd0; N_sync <= 32'd0;
        end else begin
            ce_d1 <= slv_reg0[0]; ce_d2 <= ce_d1; ce_d3 <= ce_d2;
            source_d1 <= slv_reg6[0]; source_d2 <= source_d1;
            N_meta <= slv_reg1; N_sync <= N_meta;
        end
    end
    wire ps_capture      = ce_d2;
    wire ps_capture_rise = ce_d2 && !ce_d3;   // P1#3: 捕获由上升沿触发（单次）

    wire [31:0] N_even = SINGLE_SLOT ? N_sync : (N_sync & ~32'd1);

    // ======================== TX sample_clk -> data_clk ========================
    // TX I/Q 在一个完整 sample_clk 周期内保持。源域每个有效样本翻转事件位；
    // 目标域同步事件并用三级寄存器采样整组总线。事件到达时三级总线已经稳定，
    // 且 245.76 MHz data_clk 对 61.44 MHz sample_clk 有足够的四拍预算。
    reg tx_event_toggle;
    always @(posedge tx_sample_clk or posedge rst) begin
        if (rst) tx_event_toggle <= 1'b0;
        else if (tx_dac_valid) tx_event_toggle <= ~tx_event_toggle;
    end

    (* ASYNC_REG = "TRUE" *) reg tx_evt_d1, tx_evt_d2, tx_evt_d3;
    reg [11:0] tx_i0_d1, tx_i0_d2, tx_i0_d3;
    reg [11:0] tx_q0_d1, tx_q0_d2, tx_q0_d3;
    reg [11:0] tx_i1_d1, tx_i1_d2, tx_i1_d3;
    reg [11:0] tx_q1_d1, tx_q1_d2, tx_q1_d3;
    reg tx_sample_seen;
    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            tx_evt_d1 <= 1'b0; tx_evt_d2 <= 1'b0; tx_evt_d3 <= 1'b0;
            tx_i0_d1 <= 12'd0; tx_i0_d2 <= 12'd0; tx_i0_d3 <= 12'd0;
            tx_q0_d1 <= 12'd0; tx_q0_d2 <= 12'd0; tx_q0_d3 <= 12'd0;
            tx_i1_d1 <= 12'd0; tx_i1_d2 <= 12'd0; tx_i1_d3 <= 12'd0;
            tx_q1_d1 <= 12'd0; tx_q1_d2 <= 12'd0; tx_q1_d3 <= 12'd0;
            tx_sample_seen <= 1'b0;
        end else begin
            tx_evt_d1 <= tx_event_toggle;
            tx_evt_d2 <= tx_evt_d1;
            tx_evt_d3 <= tx_evt_d2;
            tx_i0_d1 <= tx_dac_data_i0; tx_i0_d2 <= tx_i0_d1; tx_i0_d3 <= tx_i0_d2;
            tx_q0_d1 <= tx_dac_data_q0; tx_q0_d2 <= tx_q0_d1; tx_q0_d3 <= tx_q0_d2;
            tx_i1_d1 <= tx_dac_data_i1; tx_i1_d2 <= tx_i1_d1; tx_i1_d3 <= tx_i1_d2;
            tx_q1_d1 <= tx_dac_data_q1; tx_q1_d2 <= tx_q1_d1; tx_q1_d3 <= tx_q1_d2;
            if (tx_evt_d2 ^ tx_evt_d3) tx_sample_seen <= 1'b1;
        end
    end
    wire tx_sample_valid = tx_evt_d2 ^ tx_evt_d3;

    // Sticky RX activity indication: distinguishes an idle RX LVDS path from
    // a valid capture path before the UI requests a DMA snapshot.
    reg rx_sample_seen;
    always @(posedge data_clk or posedge rst) begin
        if (rst) rx_sample_seen <= 1'b0;
        else if (adc_valid) rx_sample_seen <= 1'b1;
    end

    // 数据源只在 packer 空闲时更新；一次 DMA 捕获内绝不会中途换源。
    reg source_active;
    wire monitor_valid = source_active ? tx_sample_valid : adc_valid;
    wire [11:0] monitor_i0 = source_active ? tx_i0_d3 : adc_data_i0;
    wire [11:0] monitor_q0 = source_active ? tx_q0_d3 : adc_data_q0;
    wire [11:0] monitor_i1 = source_active ? tx_i1_d3 : adc_data_i1;
    wire [11:0] monitor_q1 = source_active ? tx_q1_d3 : adc_data_q1;

    // ---- 64bit 时间戳（data_clk 域自由计数） ----
    reg [63:0] ts_cnt;
    always @(posedge data_clk or posedge rst) begin
        if (rst) ts_cnt <= 64'd0;
        else     ts_cnt <= ts_cnt + 1'b1;
    end
    reg [63:0] ts_start;

    // ---- 采集状态机 ----
    // 流水线：每拍最多提交 1 字（tvalid_r && tready）+ 同拍装载下一字。
    // 装载 ch1 时快照整组（i0,q0,i1,q0），ch2 用快照 → 背压延迟不丢不错。
    // is_ch2 = 当前挂起的字是 ch2。
    // tlast 在装载 ch2（第 wcnt+2 字）且 wcnt+2==N_even 时置；
    // done 在提交 ch2（第 wcnt+1 字）且 wcnt+1==N_even 时置。
    localparam ST_IDLE = 2'd0, ST_CAPT = 2'd1;
    reg [1:0] state;
    reg [31:0] wcnt;          // 已提交 32bit 字数
    // Precomputed terminal counts.  Comparing against registered constants
    // avoids putting a 32-bit add/compare carry chain on the 245.76 MHz RX
    // path (the former 40 MHz implementation used wcnt+1/wcnt+2 directly).
    reg [31:0] last_word_index;
    reg [31:0] ch2_load_index;
    reg done_r, overflow_r;
    reg tvalid_r, tlast_r;
    reg [31:0] tdata_r;
    reg is_ch2;               // 挂起的字是 ch2
    reg [11:0] cap_i0, cap_q0, cap_i1, cap_q1;   // 整组快照
    reg rx_cadence_seen;
    reg [2:0] rx_gap_count;
    // 接收帧错误可使adc_valid少一拍；不允许静默拼接缺样块。
    wire rx_gap_error = SINGLE_SLOT && state == ST_CAPT && !source_active &&
        rx_cadence_seen && ((adc_valid && rx_gap_count != 3'd1) ||
                           (!adc_valid && rx_gap_count >= 3'd2));
    always @(posedge data_clk or posedge rst) begin
        if (rst) begin rx_cadence_seen <= 0; rx_gap_count <= 0; end
        else if (state != ST_CAPT || source_active || !ps_capture) begin
            rx_cadence_seen <= 0; rx_gap_count <= 0;
        end else if (adc_valid) begin
            rx_cadence_seen <= 1; rx_gap_count <= 0;
        end else if (rx_gap_count != 3'd7) rx_gap_count <= rx_gap_count + 1'b1;
    end

    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            state <= ST_IDLE; wcnt <= 32'd0;
            last_word_index <= 32'd0; ch2_load_index <= 32'd0;
            done_r <= 1'b0; overflow_r <= 1'b0;
            tvalid_r <= 1'b0; tlast_r <= 1'b0; tdata_r <= 32'd0;
            is_ch2 <= 1'b0; ts_start <= 64'd0;
            source_active <= 1'b1;
            cap_i0 <= 12'd0; cap_q0 <= 12'd0; cap_i1 <= 12'd0; cap_q1 <= 12'd0;
        end else begin
            // AXIS背压时TLAST与TDATA/TVALID一起保持，直到最后一字被接收。
            if (!tvalid_r || m_axis_tready) tlast_r <= 1'b0;
            case (state)
            ST_IDLE: begin
                if (!ps_capture) source_active <= TX_ONLY ? 1'b1 : source_d2;
                // done 保持到 PS 清 capture_en（读 STATUS 后清 CTRL）
                if (!ps_capture) begin
                    done_r <= 1'b0;
                    wcnt <= 32'd0;   // wcnt 在 PS 清 capture_en 前保持 → CNT 读回有效
                    overflow_r <= 1'b0;
                end
                // 与done一起保持到PS清CTRL，否则DMA IRQ之后读不到背压。
                tvalid_r <= 1'b0;
                is_ch2 <= 1'b0;
                if (ps_capture_rise) begin
                    if (N_even == 32'd0) begin
                        done_r <= 1'b1;     // P2#4: N=0 空捕获
                    end else begin
                        state <= ST_CAPT;
                        last_word_index <= N_even - 1'b1;
                        ch2_load_index <= N_even - 2'd2;
                        ts_start <= ts_cnt; // 捕获起点时间戳（start 边锁存）
                    end
                end
            end
            ST_CAPT: begin
                // PS may cancel a monitor capture (STR0/source switch or a
                // DMA timeout recovery).  The previous FSM ignored
                // capture_en once ST_CAPT had started; if the selected source
                // stopped producing valid samples the packer stayed armed
                // forever and every later capture/source change appeared
                // dead.  Dropping capture_en now aborts the partial AXIS
                // frame immediately.  Software resets S2MM before re-arming,
                // so the partial frame is never exposed as a valid capture.
                if (!ps_capture) begin
                    state <= ST_IDLE;
                    done_r <= 1'b0;
                    tvalid_r <= 1'b0;
                    tlast_r <= 1'b0;
                    is_ch2 <= 1'b0;
                end else if (tvalid_r && m_axis_tready) begin
                    // 提交当前字
                    wcnt <= wcnt + 1'b1;
                    if ((SINGLE_SLOT || is_ch2) && (wcnt == last_word_index)) begin
                        // 提交最后一字（ch2）→ done，不装载
                        done_r <= 1'b1;
                        state <= ST_IDLE;
                        tvalid_r <= 1'b0;
                    end else if (SINGLE_SLOT || is_ch2) begin
                        // ch2 已提交（非末字，理论不出现），等新组 ch1
                        if (monitor_valid) begin
                            cap_i0 <= monitor_i0; cap_q0 <= monitor_q0;
                            cap_i1 <= monitor_i1; cap_q1 <= monitor_q1;
                            tdata_r <= {sext(monitor_q0), sext(monitor_i0)};
                            tvalid_r <= 1'b1; is_ch2 <= 1'b0;
                            if (SINGLE_SLOT) tlast_r <= (wcnt == ch2_load_index);
                        end else begin
                            tvalid_r <= 1'b0;
                        end
                    end else begin
                        // ch1 已提交 → 立即装载同组 ch2（快照）
                        tdata_r <= {sext(cap_q1), sext(cap_i1)};
                        tvalid_r <= 1'b1;
                        tlast_r <= (wcnt == ch2_load_index);
                        is_ch2 <= 1'b1;
                    end
                end
                else if (!tvalid_r) begin
                    // 无挂起字：装载新组 ch1（当前选择源有效）
                    if (monitor_valid) begin
                        cap_i0 <= monitor_i0; cap_q0 <= monitor_q0;
                        cap_i1 <= monitor_i1; cap_q1 <= monitor_q1;
                        tdata_r <= {sext(monitor_q0), sext(monitor_i0)};
                        tvalid_r <= 1'b1; is_ch2 <= 1'b0;
                        if (SINGLE_SLOT) tlast_r <= (wcnt == last_word_index);
                    end
                end
                // 背压指示（脉冲，非丢数据）：有字挂起但下游未接收
                if ((tvalid_r && !m_axis_tready) || rx_gap_error) overflow_r <= 1'b1;
            end
            endcase
        end
    end

    assign m_axis_tdata  = tdata_r;
    assign m_axis_tvalid = tvalid_r;
    assign m_axis_tlast  = tlast_r;

    // ---- 时间戳 2-FF 同步到 100MHz S_AXI_ACLK 域（P3#7） ----
    (* ASYNC_REG = "TRUE" *) reg [31:0] ts0_s1, ts0_s2, ts1_s1, ts1_s2;
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            ts0_s1 <= 32'd0; ts0_s2 <= 32'd0; ts1_s1 <= 32'd0; ts1_s2 <= 32'd0;
        end else begin
            ts0_s1 <= ts_start[31:0];  ts0_s2 <= ts0_s1;
            ts1_s1 <= ts_start[63:32]; ts1_s2 <= ts1_s1;
        end
    end

    // 状态源端先寄存，避免组合逻辑直接进入异步同步器。
    reg [5:0] status_data;
    reg [31:0] wcnt_gray_data;
    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            status_data <= 6'd0;
            wcnt_gray_data <= 32'd0;
        end else begin
            status_data <= {rx_sample_seen, tx_sample_seen, source_active,
                            (state == ST_CAPT), overflow_r, done_r};
            wcnt_gray_data <= wcnt ^ (wcnt >> 1);
        end
    end
    (* ASYNC_REG = "TRUE" *) reg [5:0] status_meta, status_sync;
    (* ASYNC_REG = "TRUE" *) reg [31:0] wcnt_gray_meta, wcnt_gray_sync;
    function [31:0] gray_to_binary;
        input [31:0] gray;
        integer bit_index;
        begin
            gray_to_binary[31] = gray[31];
            for (bit_index = 30; bit_index >= 0; bit_index = bit_index - 1)
                gray_to_binary[bit_index] = gray_to_binary[bit_index+1] ^ gray[bit_index];
        end
    endfunction
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            status_meta <= 6'd0; status_sync <= 6'd0;
            wcnt_gray_meta <= 32'd0; wcnt_gray_sync <= 32'd0;
        end else begin
            status_meta <= status_data; status_sync <= status_meta;
            wcnt_gray_meta <= wcnt_gray_data; wcnt_gray_sync <= wcnt_gray_meta;
        end
    end
    // 已完成捕获的计数/时间戳保持稳定，PS看到同步done后再读取。
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            slv_reg2 <= 32'd0; slv_reg3 <= 32'd0; slv_reg4 <= 32'd0; slv_reg5 <= 32'd0;
        end else begin
            slv_reg2[5:0] <= status_sync;
            slv_reg2[31:6] <= 26'd0;
            slv_reg3 <= gray_to_binary(wcnt_gray_sync);
            slv_reg4 <= ts0_s2;
            slv_reg5 <= ts1_s2;
        end
    end

    // V7接收通路旁路监视DMA，不改变原IQ打包/发射功能。
    // 控制总线在停止时稳定；Enable经三级后才打开DSP，参数已过两级。
    (* ASYNC_REG="TRUE" *) reg [6:0] rx_cfg_m=0,rx_cfg_s=0,rx_cfg_t=0;
    (* ASYNC_REG="TRUE" *) reg [31:0] rx_rate_m,rx_rate_s,rx_sync_m,rx_sync_s,rx_frame_m,rx_frame_s;
    (* ASYNC_REG="TRUE" *) reg [23:0] rx_recip_m,rx_recip_s;
    always @(posedge data_clk) begin
        if(rst) begin
            rx_cfg_m<=0;rx_cfg_s<=0;rx_cfg_t<=0;
            rx_rate_m<=279620267;rx_rate_s<=279620267;
            rx_sync_m<=32'hfe6b2840;rx_sync_s<=32'hfe6b2840;
            rx_frame_m<=32'h80011000;rx_frame_s<=32'h80011000;rx_recip_m<=251658;rx_recip_s<=251658;
        end
        else begin
            rx_cfg_m<=rx_ctrl[6:0];rx_cfg_s<=rx_cfg_m;rx_cfg_t<=rx_cfg_s;
            rx_rate_m<=rx_rate;rx_rate_s<=rx_rate_m;
            rx_sync_m<=rx_sync_word;rx_sync_s<=rx_sync_m;
            rx_frame_m<=rx_frame_bits;rx_frame_s<=rx_frame_m;
            rx_recip_m<=rx_recip;rx_recip_s<=rx_recip_m;
        end
    end
    wire rx_lock,rx_fault,rx_sv,rx_sb;wire [4:0] rx_scan;
    wire [31:0] rx_bit_count,rx_frames,rx_payload_count,rx_error_count;
    wire signed [31:0] rx_trim_q24;wire [31:0] rx_adjusted_rate;
    wire rx_pn_aligned;wire [17:0] rx_sfid_status;
    reg rx_fifo_overflow=0;
    p351_rx_dsp receiver(.clk(data_clk),.reset(rst),.enable(rx_cfg_t[0]),.halt(rx_fifo_overflow),
        .soq_mode(rx_cfg_t[1]),.randomized(rx_cfg_t[2]),.rate_word(rx_rate_s),.resample_recip(rx_recip_s),
        .line_code(rx_cfg_t[5:3]),.sync_bits(rx_frame_s[31:26]),.major_count(rx_frame_s[24:16]),.sfid_start(rx_frame_s[25]),
        .sync_word({rx_cfg_t[6],rx_sync_s}),.frame_bits(rx_frame_s[15:0]),.adc_valid(adc_valid),
        .adc_i(adc_data_i0),.adc_q(adc_data_q0),.locked(rx_lock),.fault(rx_fault),.scan_phase(rx_scan),
        .bit_count(rx_bit_count),.frame_count(rx_frames),.payload_count(rx_payload_count),.error_count(rx_error_count),
        .stream_valid(rx_sv),.stream_bit(rx_sb),.pn_aligned(rx_pn_aligned),.sfid_status(rx_sfid_status),
        .timing_trim_q24(rx_trim_q24),.timing_rate_word(rx_adjusted_rate));
    // 诊断的微调量不是单步Gray计数，必须先源端锁存再请求/应答搬运。
    (* ASYNC_REG="TRUE" *) reg timing_request_m=0,timing_request_s=0,timing_request_t=0;
    // Q40积分右移16位只剩26位有符号数。只搬运这26位，目的域再扩展，
    // 避免同一个符号位扇出到多个异步同步链，形成CDC-11再汇合风险。
    reg timing_ack_data=0;reg [75:0] timing_snapshot_data=0;
    always @(posedge data_clk) begin
        if(rst)begin timing_request_m<=0;timing_request_s<=0;timing_request_t<=0;timing_ack_data<=0;timing_snapshot_data<=0;end
        else begin
            timing_request_m<=rx_timing_request;timing_request_s<=timing_request_m;timing_request_t<=timing_request_s;
            if(timing_request_t!=timing_ack_data)begin
                timing_snapshot_data<={rx_sfid_status,rx_adjusted_rate,rx_trim_q24[25:0]};timing_ack_data<=timing_request_t;
            end
        end
    end
    (* ASYNC_REG="TRUE" *) reg timing_ack_m=0,timing_ack_s=0;
    (* ASYNC_REG="TRUE" *) reg [75:0] timing_meta=0,timing_sync=0;
    reg [81:0] timing_shadow=0;
    always @(posedge S_AXI_ACLK)begin
        if(!S_AXI_ARESETN)begin timing_ack_m<=0;timing_ack_s<=0;timing_meta<=0;timing_sync<=0;timing_shadow<=0;end
        else begin
            timing_ack_m<=timing_ack_data;timing_ack_s<=timing_ack_m;
            timing_meta<=timing_snapshot_data;timing_sync<=timing_meta;
            if(timing_ack_s==rx_timing_request)
                timing_shadow<={timing_sync[75:26],{6{timing_sync[25]}},timing_sync[25:0]};
        end
    end
    wire timing_ready=timing_ack_s==rx_timing_request;
    // 异步FIFO：1024个32bit字，MSB先接收；满时锁存错误，不伪装连续位流。
    reg [31:0] rx_fifo[0:1023];
    reg [10:0] rx_wr_bin=0,rx_wr_gray=0,rx_rd_bin=0,rx_rd_gray=0;
    (* ASYNC_REG="TRUE" *) reg [10:0] rx_rd_m=0,rx_rd_s=0,rx_wr_m=0,rx_wr_s=0;
    reg [31:0] rx_bit_shift=0;reg [4:0] rx_word_bits=0;
    wire rx_full=rx_wr_gray=={~rx_rd_s[10:9],rx_rd_s[8:0]};
    wire [10:0] rx_wr_next=rx_wr_bin+1'b1,rx_rd_next=rx_rd_bin+1'b1;
    wire rx_pop=!axi_rvalid && S_AXI_ARVALID && S_AXI_ARADDR[5:2]==4'hd && !rx_fifo_empty;
    always @(posedge data_clk) begin
        if(rst || !rx_cfg_t[0]) begin
            rx_wr_bin<=0;rx_wr_gray<=0;rx_rd_m<=0;rx_rd_s<=0;rx_word_bits<=0;rx_fifo_overflow<=0;
        end else begin
            rx_rd_m<=rx_rd_gray;rx_rd_s<=rx_rd_m;
            if(rx_sv && !rx_fifo_overflow) begin
                rx_bit_shift<={rx_bit_shift[30:0],rx_sb};rx_word_bits<=rx_word_bits+1'b1;
                if(rx_word_bits==31) begin
                    if(rx_full) rx_fifo_overflow<=1;
                    else begin
                        rx_fifo[rx_wr_bin[9:0]]<={rx_bit_shift[30:0],rx_sb};
                        rx_wr_bin<=rx_wr_next;rx_wr_gray<=rx_wr_next^(rx_wr_next>>1);
                    end
                end
            end
        end
    end
    reg [31:0] rx_rd_data=0;
    always @(posedge S_AXI_ACLK) begin
        rx_rd_data<=rx_fifo[rx_rd_bin[9:0]];
        if(!S_AXI_ARESETN || !rx_ctrl[0]) begin rx_rd_bin<=0;rx_rd_gray<=0;rx_wr_m<=0;rx_wr_s<=0;end
        else begin
            rx_wr_m<=rx_wr_gray;rx_wr_s<=rx_wr_m;
            if(rx_pop) begin rx_rd_bin<=rx_rd_next;rx_rd_gray<=rx_rd_next^(rx_rd_next>>1);end
        end
    end
    assign rx_fifo_data=rx_rd_data;assign rx_fifo_empty=rx_wr_s==rx_rd_gray;
    (* ASYNC_REG="TRUE" *) reg [9:0] rx_st_m=0,rx_st_s=0;
    reg [31:0] rx_b_gray=0,rx_f_gray=0,rx_p_gray=0,rx_e_gray=0;
    always @(posedge data_clk) begin
        rx_b_gray<=rx_bit_count^(rx_bit_count>>1);rx_f_gray<=rx_frames^(rx_frames>>1);
        rx_p_gray<=rx_payload_count^(rx_payload_count>>1);rx_e_gray<=rx_error_count^(rx_error_count>>1);
    end
    (* ASYNC_REG="TRUE" *) reg [31:0] rx_b_m,rx_b_s,rx_f_m,rx_f_s,rx_p_m,rx_p_s,rx_e_m,rx_e_s;
    always @(posedge S_AXI_ACLK) begin
        if(!S_AXI_ARESETN) begin
            rx_st_m<=0;rx_st_s<=0;rx_b_m<=0;rx_b_s<=0;rx_f_m<=0;rx_f_s<=0;
            rx_p_m<=0;rx_p_s<=0;rx_e_m<=0;rx_e_s<=0;
        end else begin
            rx_st_m<={rx_pn_aligned,rx_cfg_t[1],rx_scan,rx_fifo_overflow,rx_fault,rx_lock};rx_st_s<=rx_st_m;
            rx_b_m<=rx_b_gray;rx_b_s<=rx_b_m;rx_f_m<=rx_f_gray;rx_f_s<=rx_f_m;
            rx_p_m<=rx_p_gray;rx_p_s<=rx_p_m;rx_e_m<=rx_e_gray;rx_e_s<=rx_e_m;
        end
    end
    (* ASYNC_REG="TRUE" *) reg rx_en_m=0,rx_en_s=0;
    always @(posedge S_AXI_ACLK) begin rx_en_m<=rx_cfg_t[0];rx_en_s<=rx_en_m;end
    wire [10:0] rx_words=gray_to_binary({21'd0,rx_wr_s})-rx_rd_bin;
    assign rx_dsp_status={6'd0,rx_words,2'd0,rx_st_s[7:3],rx_st_s[8],rx_st_s[9],timing_ready,rx_fifo_empty,
        rx_st_s[2:0],rx_en_s};
    assign rx_dsp_counter=rx_counter_select==0?gray_to_binary(rx_b_s):
        rx_counter_select==1?gray_to_binary(rx_f_s):rx_counter_select==2?gray_to_binary(rx_p_s):
        rx_counter_select==3?gray_to_binary(rx_e_s):rx_counter_select==4?timing_shadow[31:0]:
        rx_counter_select==5?timing_shadow[63:32]:rx_counter_select==6?{14'd0,timing_shadow[81:64]}:
        rx_counter_select==7?32'h52395831:32'd0;

endmodule
