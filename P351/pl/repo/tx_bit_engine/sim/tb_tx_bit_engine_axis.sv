`timescale 1ns/1ps

module tb_tx_bit_engine_axis;
    reg clk = 1'b0;
    reg resetn = 1'b0;
    reg start = 1'b0;
    reg stop = 1'b0;
    reg clear = 1'b0;
    reg [31:0] bit_count = 0;
    reg [31:0] bit_rate = 100;
    reg [31:0] rate_word = 32'hFFFF_FFFF;
    reg [31:0] repeat_count = 0;
    reg [31:0] tdata = 0;
    reg [3:0] tkeep = 0;
    reg tvalid = 0;
    wire tready;
    reg tlast = 0;
    wire bit_data, bit_valid, frame_start, frame_end;
    reg bit_ready = 1'b1;
    wire busy, done, underrun, error;
    wire [7:0] error_code;
    wire [31:0] bits_sent;
    wire [31:0] frames_completed;
    wire [31:0] bit_phase;

    integer failures = 0;
    integer observed = 0;
    reg [255:0] expected;
    integer expected_count = 0;
    integer expect_end_on_last = 1;
    integer cycle_count = 0;
    integer last_bit_cycle = 0;
    integer check_rate_spacing = 0;
    integer check_fractional_spacing = 0;
    integer saw_short_spacing = 0;
    integer saw_long_spacing = 0;

    always #5 clk = ~clk;

    tx_bit_engine_axis #(.AXIS_CLK_HZ(100)) dut (
        .axis_clk(clk), .axis_aresetn(resetn),
        .start_pulse(start), .stop_pulse(stop), .clear_pulse(clear),
        .cfg_bit_count(bit_count), .cfg_repeat_count(repeat_count),
        .cfg_bit_rate(bit_rate), .cfg_rate_word(rate_word), .s_axis_tdata(tdata),
        .s_axis_tkeep(tkeep), .s_axis_tvalid(tvalid),
        .s_axis_tready(tready), .s_axis_tlast(tlast),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .frame_start(frame_start), .frame_end(frame_end),
        .busy(busy), .done(done), .underrun(underrun), .error(error),
        .error_code(error_code), .bits_sent(bits_sent),
        .frames_completed(frames_completed), .bit_phase(bit_phase)
    );

    always @(posedge clk) begin
        cycle_count = cycle_count + 1;
        if (bit_valid && bit_ready) begin
            if (check_rate_spacing && observed > 0 && cycle_count - last_bit_cycle != 4) begin
                $display("ERROR: rate spacing expected 4 clocks, got %0d",
                         cycle_count - last_bit_cycle);
                failures = failures + 1;
            end
            if (check_fractional_spacing && observed > 0) begin
                if (cycle_count-last_bit_cycle == 3) saw_short_spacing = 1;
                else if (cycle_count-last_bit_cycle == 4) saw_long_spacing = 1;
                else begin
                    $display("ERROR: fractional spacing expected 3/4 clocks, got %0d",
                             cycle_count-last_bit_cycle);
                    failures = failures + 1;
                end
            end
            last_bit_cycle = cycle_count;
            if (observed >= expected_count) begin
                $display("ERROR: unexpected extra bit %0d", bit_data);
                failures = failures + 1;
            end else if (bit_data !== expected[expected_count-1-observed]) begin
                $display("ERROR: bit %0d expected %0d got %0d",
                         observed, expected[expected_count-1-observed], bit_data);
                failures = failures + 1;
            end
            if ((observed == 0) != frame_start) begin
                $display("ERROR: frame_start at bit %0d", observed);
                failures = failures + 1;
            end
            if (((observed == expected_count-1) && expect_end_on_last) != frame_end) begin
                $display("ERROR: frame_end at bit %0d", observed);
                failures = failures + 1;
            end
            observed = observed + 1;
        end
    end

    task pulse_clear;
        begin
            @(negedge clk); clear = 1'b1;
            @(negedge clk); clear = 1'b0;
        end
    endtask

    task pulse_start;
        input [31:0] count;
        begin
            bit_count = count;
            observed = 0;
            @(negedge clk); start = 1'b1;
            @(negedge clk); start = 1'b0;
        end
    endtask

    task send_beat;
        input [31:0] data_value;
        input [3:0] keep_value;
        input last_value;
        begin
            @(negedge clk);
            tdata = data_value; tkeep = keep_value;
            tlast = last_value; tvalid = 1'b1;
            while (!tready) @(negedge clk);
            @(negedge clk);
            tvalid = 1'b0; tlast = 1'b0; tkeep = 4'd0;
        end
    endtask

    task wait_done;
        integer timeout;
        begin
            timeout = 0;
            while (!done && timeout < 500) begin
                @(negedge clk); timeout = timeout + 1;
            end
            if (!done) begin
                $display("ERROR: timeout waiting for done, code=%02x", error_code);
                failures = failures + 1;
            end
            if (observed != expected_count) begin
                $display("ERROR: expected %0d bits, observed %0d", expected_count, observed);
                failures = failures + 1;
            end
        end
    endtask

    initial begin
        repeat (5) @(negedge clk);
        resetn = 1'b1;

        // 16 bits: AXI lane0 AA, lane1 55, each byte MSB first.
        expected_count = 16; expected = 16'hAA55;
        expect_end_on_last = 1;
        pulse_start(16);
        send_beat(32'h0000_55AA, 4'b0011, 1'b1);
        wait_done();
        if (error || bits_sent != 16) begin
            $display("ERROR: valid 16-bit case status"); failures = failures + 1;
        end

        // Non-byte-aligned: only the first bit of the final byte is valid.
        pulse_clear();
        expected_count = 9; expected = 9'b101010101;
        expect_end_on_last = 1;
        pulse_start(9);
        send_beat(32'h0000_D5AA, 4'b0011, 1'b1);
        wait_done();
        if (error || bits_sent != 9) begin
            $display("ERROR: valid 9-bit case status"); failures = failures + 1;
        end

        // Backpressure must not change the bit order or count.
        pulse_clear();
        expected_count = 8; expected = 8'hC3;
        expect_end_on_last = 1;
        pulse_start(8);
        fork
            send_beat(32'h0000_00C3, 4'b0001, 1'b1);
            begin
                wait(bit_valid); @(negedge clk); bit_ready = 1'b0;
                repeat (4) @(negedge clk); bit_ready = 1'b1;
            end
        join
        wait_done();
        if (error || bits_sent != 8) begin
            $display("ERROR: backpressure case status"); failures = failures + 1;
        end

        // Early TLAST: 8 supplied bits while 16 were configured.
        pulse_clear();
        expected_count = 8; expected = 8'hF0;
        expect_end_on_last = 0;
        pulse_start(16);
        send_beat(32'h0000_00F0, 4'b0001, 1'b1);
        repeat (20) @(negedge clk);
        if (!underrun || !error || error_code != 8'h03 || bits_sent != 8) begin
            $display("ERROR: early TLAST not detected, code=%02x", error_code);
            failures = failures + 1;
        end

        // Missing TLAST is a protocol error even when BIT_COUNT is met.
        pulse_clear();
        expected_count = 8; expected = 8'h5A;
        expect_end_on_last = 1;
        pulse_start(8);
        send_beat(32'h0000_005A, 4'b0001, 1'b0);
        wait_done();
        if (!error || error_code != 8'h04) begin
            $display("ERROR: missing TLAST not detected, code=%02x", error_code);
            failures = failures + 1;
        end

        // 25 bit/s on a 100 Hz clock: accepted BITs must be four clocks apart.
        pulse_clear();
        bit_rate = 25;
        rate_word = 32'h4000_0000;
        check_rate_spacing = 1;
        expected_count = 8; expected = 8'hA5;
        expect_end_on_last = 1;
        pulse_start(8);
        send_beat(32'h0000_00A5, 4'b0001, 1'b1);
        wait_done();
        check_rate_spacing = 0;
        if (error || bits_sent != 8) begin
            $display("ERROR: fractional rate case status"); failures = failures + 1;
        end

        // 30/100 has 3.333 clocks/BIT: residue must produce both 3 and 4.
        pulse_clear();
        bit_rate = 30; rate_word = 32'h4CCC_CCCD;
        check_fractional_spacing = 1; saw_short_spacing = 0; saw_long_spacing = 0;
        expected_count = 24; expected = 24'hA55AA5;
        pulse_start(24);
        send_beat(32'h00A5_5AA5, 4'b0111, 1'b1);
        wait_done();
        check_fractional_spacing = 0;
        if (!saw_short_spacing || !saw_long_spacing || error) begin
            $display("ERROR: fractional residue sequence missing short/long interval");
            failures = failures + 1;
        end

        // Invalid zero and above-clock rates must fail without accepting data.
        pulse_clear(); bit_rate = 0; rate_word = 0; pulse_start(8); repeat (4) @(negedge clk);
        if (!error || busy || error_code != 8'h06) begin
            $display("ERROR: zero rate not rejected"); failures = failures + 1;
        end
        pulse_clear(); bit_rate = 101; rate_word = 32'hFFFF_FFFF; pulse_start(8); repeat (4) @(negedge clk);
        if (!error || busy || error_code != 8'h07) begin
            $display("ERROR: high rate not rejected"); failures = failures + 1;
        end

        if (failures == 0) $display("PASS: tx_bit_engine_axis tests passed");
        else $display("FAIL: %0d failures", failures);
        $finish;
    end
endmodule
