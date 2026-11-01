`timescale 1ns/1ps
module tb_tfs_spi;
 reg clk=0;always #5 clk=~clk;
 reg rst=0;reg [5:0] aw=0,ar=0;reg av=0,wv=0,rv=0,br=1,rr=1;
 reg [31:0] wd=0;reg [3:0] ws=15;
 wire awready,wready,bv,arready,rvalid;wire [1:0] bresp,rresp;wire [31:0] rd;
 wire sck,le,mosi;reg miso=0,ld=0;
 wire [6:0] atten;
 tfs_spi dut(.S_AXI_ACLK(clk),.S_AXI_ARESETN(rst),.S_AXI_AWADDR(aw),.S_AXI_AWPROT(3'd0),
 .S_AXI_AWVALID(av),.S_AXI_AWREADY(awready),.S_AXI_WDATA(wd),.S_AXI_WSTRB(ws),
 .S_AXI_WVALID(wv),.S_AXI_WREADY(wready),.S_AXI_BRESP(bresp),.S_AXI_BVALID(bv),.S_AXI_BREADY(br),
 .S_AXI_ARADDR(ar),.S_AXI_ARPROT(3'd0),.S_AXI_ARVALID(rv),.S_AXI_ARREADY(arready),
 .S_AXI_RDATA(rd),.S_AXI_RRESP(rresp),.S_AXI_RVALID(rvalid),.S_AXI_RREADY(rr),
 .tfs_sck(sck),.tfs_le(le),.tfs_mosi(mosi),.tfs_miso(miso),.tfs_ld(ld),.tfs_atten_ttl(atten));
 integer edges=0;reg [87:0] observed=0;
 reg reply_enable=0;
 reg [79:0] reply_bits=0;
 integer reply_length=0;
 // Model a mode-0 slave changing data after falling edges, with 8 ns delay.
 always @(negedge sck) begin
  if(reply_enable && edges>=8 && edges<8+reply_length)
   miso <= #8 reply_bits[reply_length-1-(edges-8)];
 end
 always @(negedge le) begin edges=0;observed=0;end
 always @(posedge sck) begin
  if(le) $fatal(1,"SCK edge outside CS");
  observed={observed[86:0],mosi};edges=edges+1;
 end
 task write_reg(input [5:0] a,input [31:0] d,input bit reverse,input [1:0] response);
 begin
  // Exercise separated AW/W arrivals in both orders.
  @(negedge clk);aw=a;wd=d;
  if(reverse) begin
   wv=1;do @(posedge clk);while(!wready);@(negedge clk);wv=0;repeat(3) @(negedge clk);
   av=1;do @(posedge clk);while(!awready);@(negedge clk);av=0;
  end else begin
   av=1;do @(posedge clk);while(!awready);@(negedge clk);av=0;repeat(3) @(negedge clk);
   wv=1;do @(posedge clk);while(!wready);@(negedge clk);wv=0;
  end
  wait(bv);if(bresp!==response) $fatal(1,"BRESP at %h got %h",a,bresp);
  @(negedge clk);
 end endtask
 task read_reg(input [5:0] a,output [31:0] d);
 begin
  @(negedge clk);ar=a;rv=1;
  do @(posedge clk);while(!arready);@(negedge clk);rv=0;
  wait(rvalid);d=rd;@(negedge clk);
 end endtask
 task completed(input integer n,input [87:0] expected);
 begin
  wait(le);wait(!dut.busy);#1;
  if(edges!=n || observed!==expected) $fatal(1,"frame %d %h expected %d %h",edges,observed,n,expected);
  if(!dut.done || sck || !le) $fatal(1,"bad completion");
 end endtask
 reg [31:0] value;
 integer a;
 initial begin
  repeat(5) @(negedge clk);rst=1;repeat(5) @(negedge clk);
  if(!le || sck || dut.busy) $fatal(1,"unsafe boot");
  read_reg(0,value);if(value!=32'h54465302) $fatal(1,"ID");
  if(atten!==0) $fatal(1,"reset attenuation");
  for(a=0;a<128;a=a+1) begin
   write_reg(6'h28,a,0,0);
   read_reg(6'h28,value);
   if(atten!==a[6:0] || value!==a) $fatal(1,"TTL register/pin mismatch");
  end
  write_reg(6'h28,128,0,2);
  if(atten!==127) $fatal(1,"invalid TTL write changed output");
  ws=0;write_reg(6'h28,0,0,2);ws=15;
  if(atten!==127) $fatal(1,"byte mask ignored");
  read_reg(12,value);if(value!=5) $fatal(1,"default must be 10 MHz");
  write_reg(12,5,0,0);
  write_reg(16,32'hA43B7400,0,0);write_reg(20,32'h0000000B,1,0);
  write_reg(4,2,0,0);
  write_reg(6'h28,0,1,0);
  if(atten!==0 || !dut.busy) $fatal(1,"attenuation interfered with SPI");
  // Busy command and register writes must not corrupt the active frame.
  write_reg(4,2,1,2);write_reg(16,0,0,2);
  completed(72,88'h000002000BA43B74000000);
  write_reg(4,256,0,0);
  write_reg(16,32'h883D2000,1,0);write_reg(20,32'h00002D79,0,0);
  write_reg(4,1,1,0);completed(88,88'h0100002D79883D20000000);
  miso=1;
  write_reg(4,8'h86,0,0);completed(16,88'h8600);
  read_reg(24,value);if(value[7:0]!=255) $fatal(1,"MISO capture");
  write_reg(4,8'h81,0,0);completed(88,88'h8100000000000000000000);
  write_reg(4,8'h82,1,0);completed(72,88'h0000820000000000000000);
  reply_enable=1;reply_length=80;reply_bits=80'h00002D79883D20000000;miso=0;
  write_reg(4,8'h81,0,0);completed(88,88'h8100000000000000000000);
  if(dut.rx_result[79:0]!==reply_bits) $fatal(1,"fine read alignment at 10MHz");
  reply_length=64;reply_bits=80'h0000000BA43B74000000;miso=0;
  write_reg(4,8'h82,0,0);completed(72,88'h0000820000000000000000);
  if(dut.rx_result[63:0]!==reply_bits[63:0]) $fatal(1,"normal read alignment at 10MHz");
  reply_length=8;reply_bits=1;miso=0;
  write_reg(4,8'h86,0,0);completed(16,88'h8600);
  if(dut.rx_result[7:0]!==8'h01) $fatal(1,"lock read alignment at 10MHz");
  reply_enable=0;
  write_reg(16,0,0,0);write_reg(20,0,0,0);write_reg(4,2,0,2);
  if(dut.busy) $fatal(1,"range rejection");
  write_reg(12,3,0,2);write_reg(4,8'h86,0,0);write_reg(4,512,0,0);
  if(dut.busy || !le || sck) $fatal(1,"abort");
  ld=1;repeat(4) @(posedge clk);read_reg(8,value);if(!value[3]) $fatal(1,"LD");
  // Read response must stay stable while the master stalls it.
  @(negedge clk);rr=0;ar=0;rv=1;
  @(posedge clk);@(negedge clk);rv=0;wait(rvalid);
  repeat(5) begin @(posedge clk);if(!rvalid || rd!=32'h54465302 || arready) $fatal(1,"R stall");end
  @(negedge clk);rr=1;@(negedge clk);
  // Independent buffered write response survives BREADY backpressure.
  br=0;write_reg(4,256,1,0);
  repeat(5) begin @(posedge clk);if(!bv || bresp || awready || wready) $fatal(1,"B stall");end
  @(negedge clk);br=1;@(negedge clk);
  // Reset must end a partially clocked transaction in idle.
  write_reg(4,8'h86,0,0);repeat(8) @(negedge clk);rst=0;
  repeat(3) @(negedge clk);
  if(!le || sck || dut.busy) $fatal(1,"reset during SPI");rst=1;
  if(atten!==0) $fatal(1,"reset must restore 63dB");
  $display("TFS_SPI_TEST PASS: frames, readback, AW/W ordering, busy rejection, range, abort, LD");$finish;
 end
 initial begin #5000000;$fatal(1,"timeout");end
endmodule
