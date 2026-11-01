`timescale 1ns/1ps

module tb_pcm_six_line_codes;
    reg clk = 1'b0;
    reg rst = 1'b1;
    reg enable = 1'b1;
    reg clear = 1'b0;
    reg bit_data = 1'b0;
    reg bit_valid = 1'b0;
    reg [31:0] bit_phase = 32'd0;
    reg [2:0] line_code = 3'd0;
    wire bit_ready;
    wire [11:0] data_i;
    wire [11:0] data_q;
    wire iq_valid;
    wire [31:0] accepted_bits;
    wire signed [15:0] debug_nrz;
    wire signed [15:0] debug_filter;
    wire [31:0] debug_phase;

    always #8.138 clk = ~clk;

    pcm_fm_modulator dut (
        .clk(clk), .rst(rst), .enable(enable), .clear(clear),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .bit_phase(bit_phase), .bit_rate_word(32'h0555_5555),
        .deviation_word(32'h01dd_dddd), .frequency_offset_word(32'd0),
        .amplitude(16'd26214), .filter_profile(4'd0),
        .line_code(line_code), .rolloff(3'd0),
        .data_i(data_i), .data_q(data_q), .iq_valid(iq_valid),
        .accepted_bits(accepted_bits), .debug_nrz(debug_nrz),
        .debug_filter(debug_filter), .debug_phase(debug_phase)
    );

    task begin_code;
        input [2:0] code;
        begin
            line_code = code;
            clear = 1'b1;
            bit_phase = 32'd0;
            repeat (2) @(posedge clk);
            #1 clear = 1'b0;
            wait(bit_ready); @(negedge clk);
        end
    endtask

    task check_level;
        input expected;
        input [127:0] label_text;
        begin
            if ((debug_nrz > 0) !== expected)
                $fatal(1, "%s expected=%0d debug_nrz=%0d", label_text,
                       expected, debug_nrz);
        end
    endtask

    task send_bit;
        input value;
        input expected_first;
        input expected_second;
        begin
            bit_data = value;
            bit_phase = 32'h0000_0000;
            bit_valid = 1'b1;
            @(posedge clk); #1;
            bit_valid = 1'b0;
            bit_phase = 32'h4000_0000;
            @(posedge clk); #1;
            check_level(expected_first, "first half");
            bit_phase = 32'hc000_0000;
            @(posedge clk); #1;
            check_level(expected_second, "second half");
        end
    endtask

    initial begin
        repeat (4) @(posedge clk);
        #1 rst = 1'b0;

        begin_code(3'd0);                    // NRZ-L
        send_bit(1'b1, 1'b1, 1'b1);
        send_bit(1'b0, 1'b0, 1'b0);

        begin_code(3'd1);                    // NRZ-M
        send_bit(1'b1, 1'b1, 1'b1);
        send_bit(1'b0, 1'b1, 1'b1);

        begin_code(3'd2);                    // NRZ-S
        send_bit(1'b1, 1'b0, 1'b0);
        send_bit(1'b0, 1'b1, 1'b1);

        begin_code(3'd3);                    // Bi-phase-L
        send_bit(1'b1, 1'b1, 1'b0);
        send_bit(1'b0, 1'b0, 1'b1);

        begin_code(3'd4);                    // Bi-phase-M
        send_bit(1'b1, 1'b0, 1'b1);
        send_bit(1'b0, 1'b0, 1'b1);

        begin_code(3'd5);                    // Bi-phase-S
        send_bit(1'b1, 1'b1, 1'b0);
        send_bit(1'b0, 1'b0, 1'b1);

        begin_code(3'd6);                    // RNRZ-L mapping; randomizer is upstream
        send_bit(1'b1, 1'b1, 1'b1);
        send_bit(1'b0, 1'b0, 1'b0);

        if (accepted_bits != 32'd2)
            $fatal(1, "accepted_bits=%0d expected=2", accepted_bits);
        $display("PASS PCM/FM seven selections including RNRZ-L mapping");
        $finish;
    end
endmodule
