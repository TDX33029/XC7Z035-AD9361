`timescale 1ns/1ps

module tb_tx_bit_engine_top;
    reg axi_clk = 0, axis_clk = 0;
    reg axi_resetn = 0, axis_resetn = 0;
    always #5 axi_clk = ~axi_clk;
    always #12.5 axis_clk = ~axis_clk;

    reg [5:0] awaddr = 0; reg [2:0] awprot = 0; reg awvalid = 0; wire awready;
    reg [31:0] wdata = 0; reg [3:0] wstrb = 0; reg wvalid = 0; wire wready;
    wire [1:0] bresp; wire bvalid; reg bready = 1;
    reg [5:0] araddr = 0; reg [2:0] arprot = 0; reg arvalid = 0; wire arready;
    wire [31:0] rdata; wire [1:0] rresp; wire rvalid; reg rready = 1;
    reg [31:0] tdata = 0; reg [3:0] tkeep = 0; reg tvalid = 0; wire tready; reg tlast = 0;
    wire bit_data, bit_valid, frame_start, frame_end;
    reg bit_ready = 1;
    wire busy, done, underrun, error; wire [31:0] bits_sent; wire irq;
    wire [31:0] bit_phase;
    integer bit_index = 0;
    integer failures = 0;
    reg [8:0] expected = 9'b101010101;
    reg [31:0] readback;

    tx_bit_engine_v1_0 dut (
        .s00_axi_aclk(axi_clk), .s00_axi_aresetn(axi_resetn),
        .s00_axi_awaddr(awaddr), .s00_axi_awprot(awprot),
        .s00_axi_awvalid(awvalid), .s00_axi_awready(awready),
        .s00_axi_wdata(wdata), .s00_axi_wstrb(wstrb),
        .s00_axi_wvalid(wvalid), .s00_axi_wready(wready),
        .s00_axi_bresp(bresp), .s00_axi_bvalid(bvalid), .s00_axi_bready(bready),
        .s00_axi_araddr(araddr), .s00_axi_arprot(arprot),
        .s00_axi_arvalid(arvalid), .s00_axi_arready(arready),
        .s00_axi_rdata(rdata), .s00_axi_rresp(rresp),
        .s00_axi_rvalid(rvalid), .s00_axi_rready(rready),
        .axis_clk(axis_clk), .axis_aresetn(axis_resetn),
        .s_axis_tdata(tdata), .s_axis_tkeep(tkeep),
        .s_axis_tvalid(tvalid), .s_axis_tready(tready), .s_axis_tlast(tlast),
        .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(bit_ready),
        .frame_start(frame_start), .frame_end(frame_end),
        .busy(busy), .done(done), .underrun(underrun), .error(error),
        .bits_sent(bits_sent), .bit_phase(bit_phase), .underrun_irq(irq)
    );

    always @(posedge axis_clk) begin
        if (bit_valid && bit_ready) begin
            if (bit_data !== expected[8-bit_index]) begin
                $display("ERROR: top bit %0d", bit_index); failures = failures + 1;
            end
            if ((bit_index == 0) != frame_start) failures = failures + 1;
            if ((bit_index == 8) != frame_end) failures = failures + 1;
            bit_index = bit_index + 1;
        end
    end

    task axi_write;
        input [5:0] address;
        input [31:0] value;
        begin
            @(negedge axi_clk);
            awaddr = address; awvalid = 1; wdata = value; wstrb = 4'hF; wvalid = 1;
            while (!(awready && wready)) @(negedge axi_clk);
            @(negedge axi_clk); awvalid = 0; wvalid = 0; wstrb = 0;
            while (!bvalid) @(negedge axi_clk);
            @(negedge axi_clk);
        end
    endtask

    task axi_read;
        input [5:0] address;
        output [31:0] value;
        begin
            @(negedge axi_clk); araddr = address; arvalid = 1;
            while (!arready) @(negedge axi_clk);
            @(negedge axi_clk); arvalid = 0;
            while (!rvalid) @(negedge axi_clk);
            value = rdata;
            @(negedge axi_clk);
        end
    endtask

    initial begin
        repeat(6) @(negedge axi_clk);
        axi_resetn = 1; axis_resetn = 1;
        repeat(8) @(negedge axi_clk);

        axi_write(6'h08, 32'd9);
        axi_write(6'h18, 32'd10000000);
        axi_write(6'h2C, 32'h4000_0000);
        // Hold the frame at the FIFO-facing interface before issuing START.
        tdata = 32'h0000_D5AA; tkeep = 4'b0011; tlast = 1; tvalid = 1;
        axi_write(6'h00, 32'h1);
        while (!tready) @(negedge axis_clk);
        @(negedge axis_clk); tvalid = 0; tlast = 0; tkeep = 0;
        wait(done);
        repeat(8) @(negedge axi_clk);

        axi_read(6'h04, readback);
        if (readback[3:0] != 4'b0010) begin
            $display("ERROR: STATUS=%08x", readback); failures = failures + 1;
        end
        axi_read(6'h1C, readback);
        if (readback != 9 || bit_index != 9 || bits_sent != 9) begin
            $display("ERROR: counts read=%0d observed=%0d direct=%0d",
                     readback, bit_index, bits_sent); failures = failures + 1;
        end
        if (irq) begin
            $display("ERROR: unexpected IRQ"); failures = failures + 1;
        end

        if (failures == 0) $display("PASS: tx_bit_engine top/AXI/CDC test passed");
        else $display("FAIL: %0d failures", failures);
        $finish;
    end
endmodule
