`timescale 1ns / 1ps

// AD9361 RX BIST checker for one I/Q pair.  It acquires both formats supported
// by ADI's axi_ad9361 RX monitor:
//   mode 0: device-specific overlapping I/Q PRBS
//   mode 1: standard PN9, checked as two consecutive 12-bit samples (24 bits)
module pn9_word_checker #(
    parameter integer Q_OR_I_N = 0,
    parameter integer LOCK_BITS = 64,
    parameter integer OOS_WORDS = 8
)(
    input  wire        clk,
    input  wire        rst,
    input  wire        clear,
    input  wire        valid,
    input  wire [11:0] data_i,
    input  wire [11:0] data_q,
    output reg         locked,
    output reg  [1:0]  selected_mode,
    output wire [5:0]  error_bits,
    output reg         oos_pulse
);

    wire [11:0] selected_i = Q_OR_I_N ? data_q : data_i;
    wire [11:0] selected_q = Q_OR_I_N ? data_i : data_q;
    wire [11:0] q_reverse = {selected_q[0], selected_q[1],
                             selected_q[2], selected_q[3],
                             selected_q[4], selected_q[5],
                             selected_q[6], selected_q[7],
                             selected_q[8], selected_q[9],
                             selected_q[10], selected_q[11]};
    wire iq_match = selected_i[7:0] == q_reverse[11:4];
    wire [15:0] device_word_raw = {selected_i, q_reverse[3:0]};
    // P351解包已按UG570恢复真实I/Q。ADI设备PN的I视图是canonical，
    // Q视图重构得到反向16bit字；仅Q视图反转，再应用同一PN递推。
    // 旧实现为适应错误的I/Q互换，反转了I视图；不能随修复保留该补偿。
    wire [15:0] device_word = !Q_OR_I_N ? device_word_raw :
        {device_word_raw[0],  device_word_raw[1],
         device_word_raw[2],  device_word_raw[3],
         device_word_raw[4],  device_word_raw[5],
         device_word_raw[6],  device_word_raw[7],
         device_word_raw[8],  device_word_raw[9],
         device_word_raw[10], device_word_raw[11],
         device_word_raw[12], device_word_raw[13],
         device_word_raw[14], device_word_raw[15]};

    reg [15:0] previous_device;
    reg        have_device;
    reg [6:0]  good_device;

    reg [11:0] standard_first;
    reg        standard_half;
    reg [23:0] previous_standard;
    reg        have_standard;
    reg [6:0]  good_standard;
    reg [3:0]  bad_words;

    function [15:0] device_advance;
        input [15:0] din;
        begin
            device_advance = {din[14:0], ((^din[15:4]) ^ (^din[2:1]))};
        end
    endfunction

    function [23:0] standard_advance;
        input [23:0] din;
        reg [8:0] history;
        reg next_bit;
        integer n;
        begin
            history = din[8:0];
            standard_advance = 0;
            for (n = 0; n < 24; n = n + 1) begin
                next_bit = history[8] ^ history[4];
                standard_advance[23-n] = next_bit;
                history = {history[7:0], next_bit};
            end
        end
    endfunction

    function [5:0] popcount16;
        input [15:0] din;
        integer n;
        begin
            popcount16 = 0;
            for (n = 0; n < 16; n = n + 1)
                popcount16 = popcount16 + din[n];
        end
    endfunction

    function [5:0] popcount24;
        input [23:0] din;
        integer n;
        begin
            popcount24 = 0;
            for (n = 0; n < 24; n = n + 1)
                popcount24 = popcount24 + din[n];
        end
    endfunction

    // 全零卡死输入不是有效PN；否则全零也满足递推，会误判眼图PASS。
    wire [5:0] device_errors = iq_match && device_word != 16'd0 ?
        popcount16(device_word ^ device_advance(previous_device)) : 6'd16;
    wire standard_complete = valid && standard_half;
    wire [23:0] standard_word = {standard_first, selected_i};
    wire [5:0] standard_errors =
        popcount24(standard_word ^ standard_advance(previous_standard));

    // Before lock, expose acquisition mismatches for diagnostics.  The core
    // clears these counters when all checkers lock, so they do not contaminate
    // the final BER result.
    wire [5:0] acquisition_errors =
        (standard_complete && have_standard &&
         standard_errors < device_errors) ? standard_errors :
        (have_device ? device_errors : 6'd0);
    wire [5:0] selected_errors = selected_mode[0] ?
        ((standard_complete && have_standard) ? standard_errors : 6'd0) :
        (have_device ? device_errors : 6'd0);
    assign error_bits = locked ? selected_errors : acquisition_errors;

    always @(posedge clk or posedge rst) begin
        if (rst || clear) begin
            previous_device <= 0;
            have_device <= 1'b0;
            good_device <= 0;
            standard_first <= 0;
            standard_half <= 1'b0;
            previous_standard <= 0;
            have_standard <= 1'b0;
            good_standard <= 0;
            locked <= 1'b0;
            selected_mode <= 0;
            bad_words <= 0;
            oos_pulse <= 1'b0;
        end else begin
            oos_pulse <= 1'b0;
            if (valid) begin
                previous_device <= device_word;
                have_device <= 1'b1;

                if (!standard_half) begin
                    standard_first <= selected_i;
                    standard_half <= 1'b1;
                end else begin
                    previous_standard <= standard_word;
                    have_standard <= 1'b1;
                    standard_half <= 1'b0;
                end

                if (!locked) begin
                    if (have_device && device_errors == 0) begin
                        if (good_device < LOCK_BITS)
                            good_device <= good_device + 1'b1;
                    end else begin
                        good_device <= 0;
                    end

                    if (standard_complete) begin
                        if (have_standard && standard_word != 24'd0 && standard_errors == 0) begin
                            if (good_standard < LOCK_BITS)
                                good_standard <= good_standard + 1'b1;
                        end else begin
                            good_standard <= 0;
                        end
                    end

                    if (have_device && device_errors == 0 &&
                        good_device >= LOCK_BITS-1) begin
                        locked <= 1'b1;
                        selected_mode <= 2'd0;
                        bad_words <= 0;
                    end else if (standard_complete && have_standard && standard_word != 24'd0 &&
                                 standard_errors == 0 &&
                                 good_standard >= LOCK_BITS-1) begin
                        locked <= 1'b1;
                        selected_mode <= 2'd1;
                        bad_words <= 0;
                    end
                end else if (selected_errors == 0) begin
                    bad_words <= 0;
                end else if (bad_words == OOS_WORDS-1) begin
                    locked <= 1'b0;
                    have_device <= 1'b0;
                    have_standard <= 1'b0;
                    standard_half <= 1'b0;
                    good_device <= 0;
                    good_standard <= 0;
                    bad_words <= 0;
                    oos_pulse <= 1'b1;
                end else begin
                    bad_words <= bad_words + 1'b1;
                end
            end
        end
    end
endmodule
