`timescale 1ns/1ps

module tb_tx_rx_monitor;
    parameter TX_ONLY = 0;
    parameter SINGLE_SLOT = 0;
    parameter TX_WORDS = 8;
    parameter real DATA_HALF = 4.069; // 122.88 MHz
    parameter real TX_HALF = 8.138;   // 61.44 MHz
    reg axi_clk = 0, data_clk = 0, tx_clk = 0;
    always #5 axi_clk = ~axi_clk;
    always #(DATA_HALF) data_clk = ~data_clk;
    always #(TX_HALF) tx_clk = ~tx_clk;

    reg axi_resetn = 0, rst = 1;
    reg [5:0] awaddr = 0, araddr = 0;
    reg awvalid = 0, wvalid = 0, bready = 1, arvalid = 0, rready = 1;
    reg [31:0] wdata = 0;
    reg [3:0] wstrb = 4'hf;
    wire awready, wready, bvalid, arready, rvalid;
    wire [1:0] bresp, rresp;
    wire [31:0] rdata;

    reg adc_valid = 1;
    reg [11:0] adc_i0 = 12'h011, adc_q0 = 12'h022;
    reg [11:0] adc_i1 = 12'h033, adc_q1 = 12'h044;
    reg tx_valid = 1;
    reg [11:0] tx_i0 = 0, tx_q0 = 0, tx_i1 = 0, tx_q1 = 0;
    wire [31:0] axis_data;
    wire axis_valid, axis_last;
    reg axis_ready = 1;

    integer out_count = 0;
    integer errors = 0;
    reg verify_rx = 0;
    reg [15:0] ch0_i, ch0_q;
    reg [11:0] previous_tx;
    always @(negedge data_clk) if(SINGLE_SLOT) adc_valid<=~adc_valid;
    function [15:0] sext(input [11:0] x);sext={{4{x[11]}},x};endfunction

    rx2r_pack #(.TX_ONLY(TX_ONLY), .SINGLE_SLOT(SINGLE_SLOT)) dut (
        .S_AXI_ACLK(axi_clk), .S_AXI_ARESETN(axi_resetn),
        .S_AXI_AWADDR(awaddr), .S_AXI_AWVALID(awvalid), .S_AXI_AWREADY(awready),
        .S_AXI_WDATA(wdata), .S_AXI_WSTRB(wstrb), .S_AXI_WVALID(wvalid),
        .S_AXI_WREADY(wready), .S_AXI_BRESP(bresp), .S_AXI_BVALID(bvalid),
        .S_AXI_BREADY(bready), .S_AXI_ARADDR(araddr), .S_AXI_ARVALID(arvalid),
        .S_AXI_ARREADY(arready), .S_AXI_RDATA(rdata), .S_AXI_RRESP(rresp),
        .S_AXI_RVALID(rvalid), .S_AXI_RREADY(rready),
        .data_clk(data_clk), .rst(rst), .adc_valid(adc_valid),
        .adc_data_i0(adc_i0), .adc_data_q0(adc_q0),
        .adc_data_i1(adc_i1), .adc_data_q1(adc_q1),
        .tx_sample_clk(tx_clk), .tx_dac_valid(tx_valid),
        .tx_dac_data_i0(tx_i0), .tx_dac_data_q0(tx_q0),
        .tx_dac_data_i1(tx_i1), .tx_dac_data_q1(tx_q1),
        .m_axis_tdata(axis_data), .m_axis_tvalid(axis_valid),
        .m_axis_tready(axis_ready), .m_axis_tlast(axis_last));

    always @(posedge tx_clk) begin
        if (!rst) begin
            tx_i0 <= tx_i0 + 1'b1;
            tx_q0 <= tx_i0 + 12'd17;
            tx_i1 <= tx_i0 + 12'd33;
            tx_q1 <= tx_i0 + 12'd49;
        end
    end

    always @(posedge data_clk) begin
        if (axis_valid && axis_ready) begin
            if (!verify_rx) begin
                if (SINGLE_SLOT || (out_count & 1) == 0) begin
                    ch0_i = axis_data[15:0];
                    ch0_q = axis_data[31:16];
                    if (SINGLE_SLOT) begin
                        if(ch0_q !== sext(ch0_i[11:0]+12'd16)) errors=errors+1;
                        if(out_count>0 && axis_data[11:0] !== (previous_tx+12'd1)) errors=errors+1;
                        previous_tx=axis_data[11:0];
                    end else if (ch0_q - ch0_i != 16) errors = errors + 1;
                end else begin
                    if (axis_data[15:0] - ch0_i != 32 ||
                        axis_data[31:16] - ch0_i != 48)
                        errors = errors + 1;
                end
            end else if (SINGLE_SLOT || (out_count & 1) == 0) begin
                if (axis_data !== 32'h00220011) errors = errors + 1;
            end else begin
                if (axis_data !== 32'h00440033) errors = errors + 1;
            end
            out_count = out_count + 1;
        end
    end

    task axi_write;
        input [5:0] addr;
        input [31:0] value;
        begin
            @(posedge axi_clk);
            awaddr <= addr; wdata <= value; awvalid <= 1; wvalid <= 1;
            while (!(awready && wready)) @(posedge axi_clk);
            @(posedge axi_clk);
            awvalid <= 0; wvalid <= 0;
            while (!bvalid) @(posedge axi_clk);
            @(posedge axi_clk);
        end
    endtask

    initial begin
        repeat (5) @(posedge axi_clk);
        rst = 0; axi_resetn = 1;
        repeat (30) @(posedge data_clk);
        if (!dut.slv_reg2[4] || !dut.slv_reg2[5])
            $fatal(1, "TX/RX sample-seen status bits did not latch");

        // Reset default is TX DAC. Capture four dual-channel sample groups.
        axi_write(6'h04, TX_WORDS);
        axi_write(6'h00, 32'd3);
        wait (out_count == TX_WORDS);
        wait (dut.slv_reg2[0]);
        if (dut.slv_reg3 !== TX_WORDS || dut.slv_reg2[1]) $fatal(1, "Synchronized capture count/BP mismatch");
        if (!axis_last) begin
            // TLAST is accepted on the eighth transfer; its pulse may be gone
            // by this delta cycle, so output-content checks remain decisive.
        end
        axi_write(6'h00, 32'd0);

        // Switch while idle to RX ADC and verify the legacy byte format.
        axi_write(6'h18, 32'd0);
        repeat (8) @(posedge data_clk);
        out_count = 0; verify_rx = !TX_ONLY;
        if (dut.slv_reg6[0] !== (TX_ONLY ? 1'b1 : 1'b0))
            $fatal(1, "SOURCE readback mismatch");
        axi_write(6'h04, 32'd4);
        axi_write(6'h00, 32'd3);
        wait (out_count == 4);
        wait (dut.slv_reg2[0]);
        if (dut.slv_reg3 !== 32'd4) $fatal(1, "RX synchronized count mismatch");

        // Cross binary carry boundaries and stall the final AXIS word.
        axi_write(6'h00, 32'd0);
        repeat (12) @(posedge data_clk);
        out_count = 0;
        axi_write(6'h04, 32'd514);
        axi_write(6'h00, 32'd3);
        wait (out_count == 513);
        @(negedge data_clk);
        axis_ready = 0;
        if(SINGLE_SLOT) wait(axis_valid && axis_last);
        if (!axis_valid || !axis_last) $fatal(1, "Missing last word before stall");
        repeat (6) begin
            @(negedge data_clk);
            if (!axis_valid || !axis_last) $fatal(1, "TLAST was lost under backpressure");
        end
        axis_ready = 1;
        wait (dut.slv_reg2[0]);
        if (out_count != 514 || dut.slv_reg3 !== 32'd514)
            $fatal(1, "Carry-boundary count/done readback mismatch");

        if (errors == 0) $display("PASS: TX/RX monitor IQ, CDC count/status and final-word backpressure");
        else $display("FAIL: %0d monitor packing errors", errors);
        if (errors != 0) $fatal(1);
        $finish;
    end

    initial begin
        #4000000;
        $fatal(1, "timeout");
    end
endmodule

module tb_tx_single_slot;
    tb_tx_rx_monitor #(.SINGLE_SLOT(1),.TX_WORDS(131072)) t();
endmodule
