`timescale 1ns/1ps
// Standalone CW source validation, no FPGA programming or IP upgrade.
module tb_cw_spectrum;
    reg clk = 0;
    reg rst = 1;
    reg [3:0] mode = 0;
    wire valid;
    wire [11:0] legacy_i, legacy_q, cw_i, cw_q;
    always #8.138020833 clk = ~clk;
    tx_waveform_channel dut(clk, rst, mode, 3'd0, valid,
                            legacy_i, legacy_q, cw_i, cw_q);
    integer fd, n, i, q, old_i, old_q;
    reg [31:0] phase;
    real ideal_i, ideal_q, err_i, err_q;
    initial begin
        fd = $fopen("cw_samples.txt", "w");
        if (!fd) $fatal(1, "Cannot create CW samples");
        repeat (4) @(negedge clk);
        rst = 0;
        mode = 6;
        repeat (16) @(negedge clk);
        // The interpolator adds one pipeline stage. Align its phase, and
        // sample the output after NBA (I and Q must have identical latency).
        for (n = 0; n < 49152; n = n + 1) begin
            phase = dut.cw_phase - 32'd139810133;
            old_i = $signed(dut.sine_lut(phase[31:24] + 8'd64)) >>> 4;
            old_q = $signed(dut.sine_lut(phase[31:24])) >>> 4;
            @(posedge clk); #1;
            i = $signed(cw_i); q = $signed(cw_q);
            ideal_i = 2047.9375 * $cos(6.283185307179586 * phase / 4294967296.0);
            ideal_q = 2047.9375 * $sin(6.283185307179586 * phase / 4294967296.0);
            err_i = i - ideal_i; err_q = q - ideal_q;
            if (!valid || err_i > 1.5 || err_i < -1.5 || err_q > 1.5 || err_q < -1.5)
                $fatal(1, "CW quadrature/rounding error n=%0d Ierr=%f Qerr=%f", n, err_i, err_q);
            $fwrite(fd, "%0d %0d %0d %0d\n", i, q, old_i, old_q);
            @(negedge clk);
        end
        $fclose(fd);
        mode = 0;
        repeat (8) @(negedge clk);
        if (legacy_i || legacy_q) $fatal(1, "OFF legacy output nonzero");
        $display("PASS: 49152 CW samples, I/Q quadrature error <= 1.5 DAC LSB");
        $finish;
    end
endmodule
