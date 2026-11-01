`timescale 1ns / 1ps

module tb_ad9361_lvds_tx_lock;
    reg rst = 1'b1;
    reg data_clk = 1'b0;
    reg sample_clk = 1'b0;
    reg sample_locked = 1'b0;
    reg dac_valid = 1'b1;
    wire tx_clk_out_p, tx_clk_out_n;
    wire tx_frame_out_p, tx_frame_out_n;
    wire [5:0] tx_data_out_p, tx_data_out_n;
    integer saw_active;
    integer saw_frame;
    integer cycle;

    always #2 data_clk = ~data_clk;
    always #8 sample_clk = ~sample_clk;

    ad9361_lvds_tx dut (
        .rst(rst), .data_clk(data_clk), .sample_clk(sample_clk),
        .sample_locked(sample_locked), .dac_valid(dac_valid),
        .dac_data_i0(12'hfff), .dac_data_q0(12'hfff),
        .dac_data_i1(12'hfff), .dac_data_q1(12'hfff),
        .tx_clk_out_p(tx_clk_out_p), .tx_clk_out_n(tx_clk_out_n),
        .tx_frame_out_p(tx_frame_out_p), .tx_frame_out_n(tx_frame_out_n),
        .tx_data_out_p(tx_data_out_p), .tx_data_out_n(tx_data_out_n)
    );

    task automatic check_active;
        begin
            saw_active = 0;
            saw_frame = 0;
            for (cycle = 0; cycle < 32; cycle = cycle + 1) begin
                @(negedge data_clk);
                #1;
                if (dut.tx_active) saw_active = 1;
                if (tx_frame_out_p) saw_frame = 1;
            end
            if (!saw_active || !saw_frame) begin
                $display("FAIL: TX did not restart after MMCM lock");
                $fatal;
            end
        end
    endtask

    initial begin
        #20 rst = 1'b0;
        #10 sample_locked = 1'b1;
        check_active();

        // Lock can drop between either clock's edges.  The serializer and
        // physical data/frame outputs must clear before the next clock edge.
        #1 sample_locked = 1'b0;
        #0.1;
        if (dut.tx_active !== 1'b0 || tx_frame_out_p !== 1'b0 ||
            tx_data_out_p !== 6'b0) begin
            $display("FAIL: stale LVDS TX data survived lock loss");
            $fatal;
        end
        #48;
        if (tx_frame_out_p !== 1'b0 || tx_data_out_p !== 6'b0) begin
            $display("FAIL: TX changed while unlocked");
            $fatal;
        end

        sample_locked = 1'b1;
        check_active();
        $display("PASS: TX data clears on lock loss and restarts after lock");
        $finish;
    end
endmodule
