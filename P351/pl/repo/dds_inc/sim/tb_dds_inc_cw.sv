`timescale 1ns/1ps

/* Unified-selector smoke test.  Mode 6 is the explicit complex sine/CW
 * source; retired mode 5, mode 0 and undefined modes must stay silent. */
module tb_dds_inc_cw;
    reg clk = 1'b0;
    reg rst = 1'b1;
    reg [31:0] phase_inc = 32'd0;
    reg bit_data = 1'b0;
    reg bit_valid = 1'b0;
    reg frame_start = 1'b0;
    reg frame_end = 1'b0;
    reg [31:0] pcm_deviation_word = 32'd0;
    reg [31:0] pcm_frequency_offset = 32'd0;
    reg [15:0] pcm_amplitude = 16'd0;
    reg [3:0] pcm_filter_profile = 4'd0;
    reg [31:0] pcm_control = 32'd0;
    reg [31:0] soqpsk_bit_rate_word = 32'd0;
    reg [31:0] artm_bit_rate_word = 32'd0;
    wire bit_ready;
    wire pcm_iq_valid;
    wire [31:0] pcm_accepted_bits;
    wire signed [15:0] pcm_debug_nrz;
    wire signed [15:0] pcm_debug_filter;
    wire [31:0] pcm_debug_phase;
    wire dac_valid;
    wire [11:0] dac_data_i0, dac_data_q0, dac_data_i1, dac_data_q1;

    always #5.0 clk = ~clk;

    dds_inc dut (
        .clk(clk), .rst(rst), .phase_inc(phase_inc),
        .bit_data(bit_data), .bit_valid(bit_valid),
        .bit_phase(32'd0),
        .frame_start(frame_start), .frame_end(frame_end),
        .bit_ready(bit_ready),
        .pcm_deviation_word(pcm_deviation_word),
        .pcm_frequency_offset(pcm_frequency_offset),
        .pcm_amplitude(pcm_amplitude), .pcm_filter_profile(pcm_filter_profile),
        .pcm_control(pcm_control),
        .soqpsk_bit_rate_word(soqpsk_bit_rate_word),
        .artm_bit_rate_word(artm_bit_rate_word),
        .pcm_iq_valid(pcm_iq_valid), .pcm_accepted_bits(pcm_accepted_bits),
        .pcm_debug_nrz(pcm_debug_nrz), .pcm_debug_filter(pcm_debug_filter),
        .pcm_debug_phase(pcm_debug_phase), .dac_valid(dac_valid),
        .dac_data_i0(dac_data_i0), .dac_data_q0(dac_data_q0),
        .dac_data_i1(dac_data_i1), .dac_data_q1(dac_data_q1)
    );

    integer n;
    reg saw_nonzero;
    reg saw_change;
    reg [11:0] prev_i;

    initial begin
        repeat (4) @(negedge clk);
        rst = 1'b0;
        phase_inc = 32'h00000006; // TX1 explicit complex sine/CW
        repeat (16) @(negedge clk);
        if (!dac_valid) $fatal(1, "dac_valid did not assert");
        saw_nonzero = 1'b0;
        saw_change = 1'b0;
        prev_i = dac_data_i0;
        for (n = 0; n < 128; n = n + 1) begin
            @(posedge clk);
            if (dac_data_i0 != 12'd0 || dac_data_q0 != 12'd0)
                saw_nonzero = 1'b1;
            if (dac_data_i0 != prev_i)
                saw_change = 1'b1;
            prev_i = dac_data_i0;
            if (dac_data_i1 != 12'd0 || dac_data_q1 != 12'd0)
                $fatal(1, "TX2 changed while OFF");
        end
        if (!saw_nonzero || !saw_change)
            $fatal(1, "mode 6 did not produce a changing sine source");

        @(negedge clk);
        phase_inc = 32'h00000600; // Only TX2 CW, TX1 must stop
        repeat (16) @(negedge clk);
        for (n = 0; n < 64; n = n + 1) begin
            @(negedge clk);
            if (dac_data_i0 !== 12'd0 || dac_data_q0 !== 12'd0)
                $fatal(1, "TX1 is not silent during TX2 CW");
            if ((dac_data_i1 === 12'd0) && (dac_data_q1 === 12'd0))
                $fatal(1, "TX2 CW I/Q both zero");
        end
        phase_inc = 32'h00000F0F; // Undefined modes must never select CW
        repeat (16) @(negedge clk);
        if (dac_data_i0 !== 12'd0 || dac_data_q0 !== 12'd0 ||
            dac_data_i1 !== 12'd0 || dac_data_q1 !== 12'd0)
            $fatal(1, "Undefined mode leaked a waveform");
        phase_inc = 32'h00000505; // Retired ARTM mode is silent on both TX paths.
        repeat (16) @(negedge clk);
        if (dac_data_i0 !== 12'd0 || dac_data_q0 !== 12'd0 ||
            dac_data_i1 !== 12'd0 || dac_data_q1 !== 12'd0 || bit_ready !== 1'b0)
            $fatal(1, "retired mode 5 still drives DAC or consumes BIT");
        phase_inc = 32'h00000000;
        repeat (16) @(negedge clk);
        if (dac_data_i0 != 12'd0 || dac_data_q0 != 12'd0)
            $fatal(1, "mode 0 is not silent");
        $display("PASS: mode 6 sine/CW; retired mode 5 and undefined modes silent");
        $finish;
    end
endmodule
