`timescale 1ns / 1ps

module lvds_prbs_checker #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
)(
    input wire data_clk,
    input wire rst,
    input wire adc_valid,
    input wire [11:0] adc_data_i0,
    input wire [11:0] adc_data_q0,
    input wire [11:0] adc_data_i1,
    input wire [11:0] adc_data_q1,

    input wire S_AXI_ACLK,
    input wire S_AXI_ARESETN,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
    input wire S_AXI_AWVALID,
    output reg S_AXI_AWREADY,
    input wire [C_S_AXI_DATA_WIDTH-1:0] S_AXI_WDATA,
    input wire [C_S_AXI_DATA_WIDTH/8-1:0] S_AXI_WSTRB,
    input wire S_AXI_WVALID,
    output reg S_AXI_WREADY,
    output wire [1:0] S_AXI_BRESP,
    output reg S_AXI_BVALID,
    input wire S_AXI_BREADY,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
    input wire S_AXI_ARVALID,
    output reg S_AXI_ARREADY,
    output reg [C_S_AXI_DATA_WIDTH-1:0] S_AXI_RDATA,
    output wire [1:0] S_AXI_RRESP,
    output reg S_AXI_RVALID,
    input wire S_AXI_RREADY
);
    assign S_AXI_BRESP = 2'b00;
    assign S_AXI_RRESP = 2'b00;

    reg aw_hold, w_hold;
    reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_hold;
    reg [31:0] wdata_hold;
    reg [3:0] wstrb_hold;
    reg enable_axi;
    reg [31:0] window_axi;
    reg clear_toggle_axi, snapshot_toggle_axi;
    integer bi;

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            S_AXI_AWREADY<=0; S_AXI_WREADY<=0; S_AXI_BVALID<=0;
            aw_hold<=0; w_hold<=0; awaddr_hold<=0; wdata_hold<=0; wstrb_hold<=0;
            enable_axi<=0; window_axi<=32'd1048576;
            clear_toggle_axi<=0; snapshot_toggle_axi<=0;
        end else begin
            S_AXI_AWREADY<=0; S_AXI_WREADY<=0;
            if (S_AXI_BVALID && S_AXI_BREADY) S_AXI_BVALID<=0;
            if (!S_AXI_BVALID) begin
                if (S_AXI_AWVALID && !aw_hold) begin S_AXI_AWREADY<=1; awaddr_hold<=S_AXI_AWADDR; aw_hold<=1; end
                if (S_AXI_WVALID && !w_hold) begin S_AXI_WREADY<=1; wdata_hold<=S_AXI_WDATA; wstrb_hold<=S_AXI_WSTRB; w_hold<=1; end
                if (aw_hold && w_hold) begin
                    case (awaddr_hold[5:2])
                        4'h0: begin
                            if (wstrb_hold[0]) begin
                                enable_axi <= wdata_hold[0];
                                if (wdata_hold[1]) clear_toggle_axi <= ~clear_toggle_axi;
                                if (wdata_hold[2]) snapshot_toggle_axi <= ~snapshot_toggle_axi;
                            end
                        end
                        4'h1: begin
                            for (bi=0;bi<4;bi=bi+1) if (wstrb_hold[bi]) window_axi[bi*8+:8] <= wdata_hold[bi*8+:8];
                        end
                        default: ;
                    endcase
                    aw_hold<=0; w_hold<=0; S_AXI_BVALID<=1;
                end
            end
        end
    end

    // AXI controls into DATA_CLK domain.  Multi-bit window is required to be
    // stable before enable rises; the two-stage sampling then forms a coherent value.
    (* ASYNC_REG="TRUE" *) reg en_s1,en_s2,clr_s1,clr_s2,clr_d,snap_s1,snap_s2;
    reg [31:0] win_s1,win_s2;
    wire clear_data = clr_s2 ^ clr_d;
    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            en_s1<=0;en_s2<=0;clr_s1<=0;clr_s2<=0;clr_d<=0;snap_s1<=0;snap_s2<=0;
            win_s1<=0;win_s2<=0;
        end else begin
            en_s1<=enable_axi; en_s2<=en_s1;
            clr_s1<=clear_toggle_axi; clr_s2<=clr_s1; clr_d<=clr_s2;
            snap_s1<=snapshot_toggle_axi; snap_s2<=snap_s1;
            win_s1<=window_axi; win_s2<=win_s1;
        end
    end

    wire snap_ack_data;
    wire [31:0] stat_snap,samp_snap,ei0_snap,eq0_snap,ei1_snap,eq1_snap,gap_snap,oos_snap;
    wire [31:0] raw0_snap,raw1_snap,raw2_snap,raw3_snap,raw4_snap;
    lvds_prbs_checker_core core (
        .data_clk(data_clk),.rst(rst),.enable(en_s2),.clear(clear_data),
        .window_samples(win_s2),.snapshot_req(snap_s2),.snapshot_ack(snap_ack_data),
        .adc_valid(adc_valid),.adc_data_i0(adc_data_i0),.adc_data_q0(adc_data_q0),
        .adc_data_i1(adc_data_i1),.adc_data_q1(adc_data_q1),
        .status_snapshot(stat_snap),.sample_snapshot(samp_snap),
        .error_i0_snapshot(ei0_snap),.error_q0_snapshot(eq0_snap),
        .error_i1_snapshot(ei1_snap),.error_q1_snapshot(eq1_snap),
        .gap_error_snapshot(gap_snap),.oos_snapshot(oos_snap),
        .raw0_snapshot(raw0_snap),.raw1_snapshot(raw1_snap),
        .raw2_snapshot(raw2_snap),.raw3_snapshot(raw3_snap),
        .raw4_snapshot(raw4_snap));

    // Snapshot payload is held stable by DATA_CLK until the next request.
    // Synchronise ack, then capture the stable payload into the AXI domain.
    (* ASYNC_REG="TRUE" *) reg ack_s1,ack_s2,ack_d;
    reg [31:0] r_status,r_samples,r_ei0,r_eq0,r_ei1,r_eq1,r_gap,r_oos;
    reg [31:0] r_raw0,r_raw1,r_raw2,r_raw3,r_raw4;
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            ack_s1<=0;ack_s2<=0;ack_d<=0;
            r_status<=0;r_samples<=0;r_ei0<=0;r_eq0<=0;r_ei1<=0;r_eq1<=0;r_gap<=0;r_oos<=0;
            r_raw0<=0;r_raw1<=0;r_raw2<=0;r_raw3<=0;r_raw4<=0;
        end else begin
            ack_s1<=snap_ack_data; ack_s2<=ack_s1; ack_d<=ack_s2;
            if (ack_s2 != ack_d) begin
                r_status<=stat_snap;r_samples<=samp_snap;r_ei0<=ei0_snap;r_eq0<=eq0_snap;
                r_ei1<=ei1_snap;r_eq1<=eq1_snap;r_gap<=gap_snap;r_oos<=oos_snap;
                r_raw0<=raw0_snap;r_raw1<=raw1_snap;r_raw2<=raw2_snap;
                r_raw3<=raw3_snap;r_raw4<=raw4_snap;
            end
        end
    end

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin S_AXI_ARREADY<=0;S_AXI_RVALID<=0;S_AXI_RDATA<=0; end
        else begin
            S_AXI_ARREADY<=0;
            if (!S_AXI_RVALID && S_AXI_ARVALID) begin
                S_AXI_ARREADY<=1;S_AXI_RVALID<=1;
                case (S_AXI_ARADDR[5:2])
                    4'h0:S_AXI_RDATA<={29'd0,snapshot_toggle_axi,1'b0,enable_axi};
                    4'h1:S_AXI_RDATA<=window_axi;
                    4'h2:S_AXI_RDATA<=r_status;
                    4'h3:S_AXI_RDATA<=r_samples;
                    4'h4:S_AXI_RDATA<=r_ei0;
                    4'h5:S_AXI_RDATA<=r_eq0;
                    4'h6:S_AXI_RDATA<=r_ei1;
                    4'h7:S_AXI_RDATA<=r_eq1;
                    4'h8:S_AXI_RDATA<=r_gap;
                    4'h9:S_AXI_RDATA<=r_oos;
                    4'hA:S_AXI_RDATA<=32'h50524253;
                    4'hB:S_AXI_RDATA<=r_raw0;
                    4'hC:S_AXI_RDATA<=r_raw1;
                    4'hD:S_AXI_RDATA<=r_raw2;
                    4'hE:S_AXI_RDATA<=r_raw3;
                    4'hF:S_AXI_RDATA<=r_raw4;
                    default:S_AXI_RDATA<=32'hDEADBEEF;
                endcase
            end else begin
                if (S_AXI_RVALID && S_AXI_RREADY) S_AXI_RVALID<=0;
            end
        end
    end
endmodule
