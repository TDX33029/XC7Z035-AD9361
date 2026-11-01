`timescale 1ns/1ps

module tb_pcm_fm_modulator;
    localparam integer FS_HZ = 61440000;
    localparam integer RB_HZ = 5000000;
    localparam [31:0] RATE_WORD = 32'd349525333;
    localparam [31:0] DEV_WORD = 32'd122333867;

    reg clk = 1'b0;
    reg rst = 1'b1;
    reg enable = 1'b0;
    reg clear = 1'b0;
    reg bit_data = 1'b1;
    wire bit_valid;
    wire bit_ready;
    wire [11:0] data_i;
    wire [11:0] data_q;
    wire iq_valid;
    wire [31:0] accepted_bits;
    wire signed [15:0] debug_nrz;
    wire signed [15:0] debug_filter;
    wire [31:0] debug_phase;
    reg [31:0] bit_phase = 32'd0;
    wire [32:0] phase_sum = {1'b0, bit_phase} + {1'b0, RATE_WORD};
    assign bit_valid = enable && !rst && phase_sum[32];

    integer cycle_count = 0;
    integer valid_count = 0;
    integer nonzero_count = 0;
    integer clip_count = 0;
    integer max_abs = 0;
    reg [15:0] pattern = 16'hAA55;
    integer pattern_index = 15;

    always #8.138020833 clk = ~clk;

    pcm_fm_modulator dut (
        .clk(clk), .rst(rst), .enable(enable), .clear(clear),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .bit_phase(bit_phase),
        .bit_rate_word(RATE_WORD),
        .deviation_word(DEV_WORD), .frequency_offset_word(32'sd0),
        .amplitude(16'd26214), .filter_profile(4'd0),
        .line_code(3'd0), .rolloff(3'd0),
        .data_i(data_i), .data_q(data_q), .iq_valid(iq_valid),
        .accepted_bits(accepted_bits), .debug_nrz(debug_nrz),
        .debug_filter(debug_filter), .debug_phase(debug_phase)
    );

    always @(posedge clk) begin
        if (!rst && enable) begin
            cycle_count <= cycle_count + 1;
            bit_phase <= phase_sum[31:0];
            if (phase_sum[32]) begin
                bit_data <= pattern[pattern_index];
                valid_count <= valid_count + 1;
                if (pattern_index == 0) pattern_index <= 15;
                else pattern_index <= pattern_index - 1;
            end
            if (iq_valid && ((data_i != 12'd0) || (data_q != 12'd0)))
                nonzero_count <= nonzero_count + 1;
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
        repeat (7000) @(posedge clk);
        if (accepted_bits < 32'd560 || accepted_bits > 32'd580)
            $fatal(1, "accepted bit count out of range: %0d", accepted_bits);
        if (nonzero_count < 6000)
            $fatal(1, "PCM/FM I/Q did not become continuously active: %0d", nonzero_count);
        if (debug_phase == 32'd0)
            $fatal(1, "phase accumulator did not run");
        if (clip_count != 0 || max_abs < 1500 || max_abs > 1700)
            $fatal(1, "PCM/FM amplitude scaling/clipping failed: clips=%0d max=%0d", clip_count, max_abs);
        $display("PASS pcm_fm: accepted=%0d nonzero=%0d I=%0d Q=%0d filter=%0d phase=%08x",
                 accepted_bits, nonzero_count, $signed(data_i), $signed(data_q),
                 debug_filter, debug_phase);
        $finish;
    end
endmodule
