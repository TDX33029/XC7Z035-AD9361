`timescale 1ns/1ps

module tb_soqpsk_tg_modulator;
    localparam integer FS_HZ = 61440000;
    localparam integer RB_HZ = 5000000;
    localparam [31:0] RATE_WORD = 32'd349525333;

    reg clk = 1'b0;
    reg rst = 1'b1;
    reg enable = 1'b0;
    reg clear = 1'b0;
    wire bit_data;
    wire bit_valid;
    wire bit_ready;
    wire [11:0] data_i;
    wire [11:0] data_q;
    wire iq_valid;
    wire [31:0] accepted_bits;
    wire signed [15:0] debug_alpha;
    wire signed [15:0] debug_frequency;
    wire [31:0] debug_phase;

    reg [31:0] bit_phase = 0;
    reg [15:0] pattern = 16'hAA55;
    integer pattern_index = 15;
    wire [32:0] rate_sum = {1'b0, bit_phase} + {1'b0, RATE_WORD};
    assign bit_valid = !rst && enable && rate_sum[32];
    assign bit_data = pattern[pattern_index];
    integer nonzero_count = 0;
    integer alpha_count = 0;
    integer frequency_count = 0;
    integer clip_count = 0;
    integer max_abs = 0;

    always #8.138020833 clk = ~clk;

    soqpsk_tg_modulator dut (
        .clk(clk), .rst(rst), .enable(enable), .clear(clear),
        .differential_enable(1'b1),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .bit_phase(bit_phase),
        .bit_rate_word(RATE_WORD), .frequency_offset_word(32'sd0),
        .amplitude(16'd26214), .data_i(data_i), .data_q(data_q),
        .iq_valid(iq_valid), .accepted_bits(accepted_bits),
        .debug_alpha(debug_alpha), .debug_frequency(debug_frequency),
        .debug_phase(debug_phase)
    );

    always @(posedge clk) begin
        if (rst || !enable) begin
            bit_phase <= 0;
        end else begin
            bit_phase <= rate_sum[31:0];
            if (bit_valid && bit_ready) begin
                if (pattern_index == 0) pattern_index <= 15;
                else pattern_index <= pattern_index - 1;
            end
            if (iq_valid && ((data_i != 12'd0) || (data_q != 12'd0)))
                nonzero_count <= nonzero_count + 1;
            if (debug_alpha != 16'sd0)
                alpha_count <= alpha_count + 1;
            if (debug_frequency != 16'sd0)
                frequency_count <= frequency_count + 1;
            if (iq_valid && (data_i == 12'h7ff || data_i == 12'h800 ||
                             data_q == 12'h7ff || data_q == 12'h800))
                clip_count <= clip_count + 1;
            if (iq_valid && (($signed(data_i) < 0 ? -$signed(data_i) : $signed(data_i)) > max_abs))
                max_abs <= ($signed(data_i) < 0 ? -$signed(data_i) : $signed(data_i));
            if (iq_valid && (($signed(data_q) < 0 ? -$signed(data_q) : $signed(data_q)) > max_abs))
                max_abs <= ($signed(data_q) < 0 ? -$signed(data_q) : $signed(data_q));
        end
    end

    initial begin
        repeat (12) @(posedge clk);
        rst <= 1'b0;
        enable <= 1'b1;
        repeat (8000) @(posedge clk);
        if (accepted_bits < 32'd640 || accepted_bits > 32'd660)
            $fatal(1, "accepted bit count out of range: %0d", accepted_bits);
        if (nonzero_count < 7500)
            $fatal(1, "SOQPSK I/Q did not become continuously active: %0d", nonzero_count);
        if (alpha_count == 0 || frequency_count == 0 || debug_phase == 32'd0)
            $fatal(1, "SOQPSK precoder/pulse/phase did not run");
        if (clip_count != 0 || max_abs < 1500 || max_abs > 1700)
            $fatal(1, "SOQPSK amplitude scaling/clipping failed: clips=%0d max=%0d", clip_count, max_abs);
        clear <= 1'b1;
        @(posedge clk);
        clear <= 1'b0;
        @(posedge clk);
        if (accepted_bits != 0)
            $fatal(1, "clear did not reset accepted_bits: %0d", accepted_bits);
        $display("PASS soqpsk_tg: nonzero=%0d alpha_cycles=%0d pulse_cycles=%0d I=%0d Q=%0d phase=%08x",
                 nonzero_count, alpha_count, frequency_count,
                 $signed(data_i), $signed(data_q), debug_phase);
        $finish;
    end
endmodule
