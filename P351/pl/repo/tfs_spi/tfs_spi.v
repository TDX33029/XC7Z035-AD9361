`timescale 1ns/1ps
// TFS21-22-25: mode 0, MSB first, active-low LE/CS. No boot transaction.
// 00 ID; 04 command (low byte opcode, bit8 clear, bit9 abort);
// 08 status [0 busy,1 done,2 error,3 synchronized LD]; 0c half-period clocks;
// 10/14 frequency low/high; 18/1c/20 received bits low/mid/high; 24 last opcode.
module tfs_spi #(
 parameter integer C_S_AXI_DATA_WIDTH=32,
 parameter integer C_S_AXI_ADDR_WIDTH=6
)(
 input wire S_AXI_ACLK, S_AXI_ARESETN,
 input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
 input wire [2:0] S_AXI_AWPROT,
 input wire S_AXI_AWVALID, output wire S_AXI_AWREADY,
 input wire [31:0] S_AXI_WDATA, input wire [3:0] S_AXI_WSTRB,
 input wire S_AXI_WVALID, output wire S_AXI_WREADY,
 output reg [1:0] S_AXI_BRESP, output reg S_AXI_BVALID, input wire S_AXI_BREADY,
 input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
 input wire [2:0] S_AXI_ARPROT,
 input wire S_AXI_ARVALID, output wire S_AXI_ARREADY,
 output reg [31:0] S_AXI_RDATA, output reg [1:0] S_AXI_RRESP,
 output reg S_AXI_RVALID, input wire S_AXI_RREADY,
 output reg tfs_sck, tfs_le, tfs_mosi,
 input wire tfs_miso, tfs_ld,
 output reg [6:0] tfs_atten_ttl
);
 reg aw_pending,w_pending;
 reg [5:0] awaddr;
 reg [31:0] wdata;
 reg [3:0] wstrb;
 assign S_AXI_AWREADY= !aw_pending && !S_AXI_BVALID;
 assign S_AXI_WREADY= !w_pending && !S_AXI_BVALID;
 assign S_AXI_ARREADY= !S_AXI_RVALID;
 reg [63:0] frequency;
 reg [15:0] divider,active_div,count;
 reg [87:0] tx_shift,rx_shift,rx_result;
 reg [6:0] remaining;
 reg [2:0] state;
 reg done,error;
 reg [7:0] last_opcode;
 wire busy=(state!=0);
 (* ASYNC_REG="TRUE" *) reg [1:0] ld_sync,miso_sync;
 integer i;
 always @(posedge S_AXI_ACLK) begin
  if(!S_AXI_ARESETN) begin
   aw_pending<=0; w_pending<=0; awaddr<=0;wdata<=0;wstrb<=0;
   S_AXI_BVALID<=0;S_AXI_BRESP<=0;S_AXI_RVALID<=0;S_AXI_RRESP<=0;S_AXI_RDATA<=0;
   frequency<=0;divider<=5;active_div<=5;count<=0;
   tx_shift<=0;rx_shift<=0;rx_result<=0;remaining<=0;state<=0;
   done<=0;error<=0;last_opcode<=0;
   tfs_sck<=0;tfs_le<=1;tfs_mosi<=0;ld_sync<=0;miso_sync<=0;
   tfs_atten_ttl<=7'b0000000; // V1..V7 low: maximum 63 dB after reset.
  end else begin
   ld_sync<={ld_sync[0],tfs_ld}; miso_sync<={miso_sync[0],tfs_miso};
   if(S_AXI_AWVALID && S_AXI_AWREADY) begin awaddr<=S_AXI_AWADDR;aw_pending<=1;end
   if(S_AXI_WVALID && S_AXI_WREADY) begin wdata<=S_AXI_WDATA;wstrb<=S_AXI_WSTRB;w_pending<=1;end
   if(S_AXI_BVALID && S_AXI_BREADY) S_AXI_BVALID<=0;
   if(S_AXI_RVALID && S_AXI_RREADY) S_AXI_RVALID<=0;
   if(S_AXI_ARVALID && S_AXI_ARREADY) begin
    S_AXI_RVALID<=1;S_AXI_RRESP<=0;
    case(S_AXI_ARADDR)
     6'h00:S_AXI_RDATA<=32'h54465302;
     6'h08:S_AXI_RDATA<={28'd0,ld_sync[1],error,done,busy};
     6'h0c:S_AXI_RDATA<={16'd0,divider};
     6'h10:S_AXI_RDATA<=frequency[31:0];
     6'h14:S_AXI_RDATA<=frequency[63:32];
     6'h18:S_AXI_RDATA<=rx_result[31:0];
     6'h1c:S_AXI_RDATA<=rx_result[63:32];
     6'h20:S_AXI_RDATA<={8'd0,rx_result[87:64]};
     6'h24:S_AXI_RDATA<={24'd0,last_opcode};
     6'h28:S_AXI_RDATA<={25'd0,tfs_atten_ttl};
     default:begin S_AXI_RDATA<=0;S_AXI_RRESP<=2;end
    endcase
   end
   // Entire frame, including post-clock hold and inter-frame gap, is bounded.
   if(busy) begin
    if(count!=0) count<=count-1'b1;
    else begin
     count<=active_div-1'b1;
     case(state)
      1:begin tfs_sck<=1;rx_shift<={rx_shift[86:0],miso_sync[1]};state<=2;end
      2:begin
       tfs_sck<=0;
       if(remaining==1) state<=3;
       else begin remaining<=remaining-1'b1;tx_shift<={tx_shift[86:0],1'b0};tfs_mosi<=tx_shift[86];state<=1;end
      end
      3:begin tfs_le<=1;tfs_mosi<=0;rx_result<=rx_shift;state<=4;end
      4:begin state<=0;done<=1;end
      default:begin state<=0;tfs_sck<=0;tfs_le<=1;error<=1;end
     endcase
    end
   end
   // AW and W are independently captured; responses survive backpressure.
   if(aw_pending && w_pending && !S_AXI_BVALID) begin
    aw_pending<=0;w_pending<=0;S_AXI_BVALID<=1;S_AXI_BRESP<=0;
    case(awaddr)
     6'h28:begin
      // One clock edge updates all seven physical levels, also during SPI.
      if(wstrb[0] && wdata[31:7]==0) tfs_atten_ttl<=wdata[6:0];
      else begin S_AXI_BRESP<=2;error<=1;end
     end
     6'h0c:begin
      if(busy || wstrb[1:0]!=2'b11 || wdata[15:0]<5) begin S_AXI_BRESP<=2;error<=1;end
      else divider<=wdata[15:0];
     end
     6'h10,6'h14:begin
      if(busy) begin S_AXI_BRESP<=2;error<=1;end
      else for(i=0;i<4;i=i+1) if(wstrb[i]) begin
       if(awaddr==6'h10) frequency[i*8+:8]<=wdata[i*8+:8];
       else frequency[32+i*8+:8]<=wdata[i*8+:8];
      end
     end
     6'h04:begin
      if(wstrb[1] && wdata[9]) begin
       state<=0;tfs_le<=1;tfs_sck<=0;tfs_mosi<=0;done<=0;error<=1;
      end else if(wstrb[1] && wdata[8]) begin done<=0;error<=0;end
      else if(wstrb[0]) begin
       if(busy || !((wdata[7:0]==8'h01 && frequency>=64'd500000000000 && frequency<=64'd226000000000000) ||
                    (wdata[7:0]==8'h02 && frequency>=64'd500000000 && frequency<=64'd226000000000) ||
                     wdata[7:0]==8'h81 || wdata[7:0]==8'h82 || wdata[7:0]==8'h86)) begin
        S_AXI_BRESP<=2;error<=1;
       end else begin
        last_opcode<=wdata[7:0];done<=0;error<=0;rx_shift<=0;
        active_div<=divider;count<=divider-1'b1;state<=1;tfs_le<=0;tfs_sck<=0;tfs_mosi<=wdata[7];
        case(wdata[7:0])
         8'h01:begin tx_shift<={8'h01,frequency,16'd0};remaining<=88;end
         8'h02:begin tx_shift<={8'h02,frequency[47:0],32'd0};remaining<=72;end
         8'h81:begin tx_shift<={8'h81,80'd0};remaining<=88;end
         8'h82:begin tx_shift<={8'h82,80'd0};remaining<=72;end
         8'h86:begin tx_shift<={8'h86,80'd0};remaining<=16;end
        endcase
       end
      end
     end
     default:begin S_AXI_BRESP<=2;error<=1;end
    endcase
   end
  end
 end
endmodule
