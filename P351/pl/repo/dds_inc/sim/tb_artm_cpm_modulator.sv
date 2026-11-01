`timescale 1ns/1ps

module tb_artm_cpm_modulator;
    localparam integer FS_HZ = 61440000;
    localparam integer RB_HZ = 5000000;
    localparam [31:0] RATE_WORD = 32'd349525333;

    reg clk = 1'b0;
    reg rst = 1'b1;
    reg enable = 1'b0;
    reg clear = 1'b0;
    reg bit_data = 1'b0;
    reg bit_valid = 1'b0;
    wire bit_ready;
    wire [11:0] data_i;
    wire [11:0] data_q;
    wire iq_valid;
    wire [31:0] accepted_bits;
    wire signed [15:0] debug_symbol;
    wire signed [15:0] debug_frequency;
    wire [31:0] debug_phase;

    integer rate_accum = 0;
    wire [31:0] bit_phase = (64'd4294967296 * rate_accum) / FS_HZ;
    integer nonzero_count = 0;
    integer frequency_count = 0;
    integer clip_count = 0;
    integer max_abs = 0;
    integer symbol_count = 0;
    reg [7:0] pattern = 8'b00011011; // Table 2-5: -3,-1,+1,+3
    integer pattern_index = 7;
    reg signed [15:0] last_symbol = 16'sd0;
    integer fractional_checks = 0;
    always @(negedge clk) begin
        if (!rst && !clear && iq_valid && !dut.have_first_bit && bit_phase != 0) begin
            if (dut.symbol_phase !== {1'b0, bit_phase[31:1]})
                $fatal(1, "ARTM discarded fractional scheduler phase");
            fractional_checks = fractional_checks + 1;
        end
    end

    always #8.138020833 clk = ~clk;

    artm_cpm_modulator dut (
        .clk(clk), .rst(rst), .enable(enable), .clear(clear),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .bit_rate_word(RATE_WORD), .frequency_offset_word(32'sd0),
        .bit_phase(bit_phase),
        .amplitude(16'd26214), .data_i(data_i), .data_q(data_q),
        .iq_valid(iq_valid), .accepted_bits(accepted_bits),
        .debug_symbol(debug_symbol), .debug_frequency(debug_frequency),
        .debug_phase(debug_phase)
    );

    always @(posedge clk) begin
        bit_valid <= 1'b0;
        if (!rst && enable) begin
            if (rate_accum + RB_HZ >= FS_HZ) begin
                rate_accum <= rate_accum + RB_HZ - FS_HZ;
                bit_data <= pattern[pattern_index];
                bit_valid <= 1'b1;
                if (pattern_index == 0) pattern_index <= 7;
                else pattern_index <= pattern_index - 1;
            end else begin
                rate_accum <= rate_accum + RB_HZ;
            end
            if (iq_valid && ((data_i != 12'd0) || (data_q != 12'd0)))
                nonzero_count <= nonzero_count + 1;
            if (debug_frequency != 16'sd0)
                frequency_count <= frequency_count + 1;
            // accepted_bits>=2 excludes reset/clear idle value (debug_symbol=0).
            if (!clear && accepted_bits >= 2 && debug_symbol != last_symbol) begin
                last_symbol <= debug_symbol;
                symbol_count <= symbol_count + 1;
                if (debug_symbol != -16'sd3 && debug_symbol != -16'sd1 &&
                    debug_symbol != 16'sd1 && debug_symbol != 16'sd3)
                    $fatal(1, "invalid ARTM symbol: %0d", debug_symbol);
            end
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
        repeat (9000) @(posedge clk);
        if (accepted_bits < 32'd720 || accepted_bits > 32'd740)
            $fatal(1, "accepted bit count out of range: %0d", accepted_bits);
        if (nonzero_count < 8500 || frequency_count == 0 || symbol_count < 100)
            $fatal(1, "ARTM chain inactive: iq=%0d pulse=%0d symbols=%0d",
                   nonzero_count, frequency_count, symbol_count);
        if (clip_count != 0 || max_abs < 1500 || max_abs > 1700)
            $fatal(1, "ARTM amplitude scaling/clipping failed: clips=%0d max=%0d", clip_count, max_abs);
        if (debug_phase == 32'd0)
            $fatal(1, "ARTM phase accumulator did not run");
        if (fractional_checks < 100)
            $fatal(1, "fractional phase not exercised");
        clear <= 1'b1;
        @(posedge clk);
        clear <= 1'b0;
        @(posedge clk);
        if (accepted_bits != 0)
            $fatal(1, "clear did not reset accepted_bits: %0d", accepted_bits);
        $display("PASS artm_cpm: iq=%0d pulse=%0d symbols=%0d max=%0d",
                 nonzero_count, frequency_count, symbol_count, max_abs);
        $finish;
    end
endmodule
