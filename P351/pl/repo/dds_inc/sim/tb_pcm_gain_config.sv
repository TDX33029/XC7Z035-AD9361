`timescale 1ns/1ps
module tb_pcm_gain_config;
reg clk=0,rst=1,clear=0,enable=1,valid=1;
reg [31:0] rate=32'h53555555,dev=32'h1d2aaaab;
wire ready;wire [31:0] count;
always #8 clk=~clk;
pcm_fm_modulator dut(.clk(clk),.rst(rst),.clear(clear),.enable(enable),
 .bit_data(1'b1),.bit_valid(valid),.bit_ready(ready),.bit_phase(32'd0),
 .bit_rate_word(rate),.deviation_word(dev),.frequency_offset_word(32'd0),
 .amplitude(16'd26214),.filter_profile(4'd0),.line_code(3'd0),.rolloff(3'd0),
 .accepted_bits(count));
task configure;
 input [31:0] r,d;
 reg [63:0] expected;
 begin
 @(negedge clk);rate=r;dev=d;clear=1;
 repeat(2)@(negedge clk);clear=0;
 repeat(60)begin
  @(negedge clk);
  if(ready||count!=0)$fatal(1,"early BIT acceptance during ratio calculation");
 end
 repeat(8)@(negedge clk);
 expected={d,32'd0}/r;
 if(!ready||dut.gain_word!=expected)$fatal(1,"ratio calculation/readiness mismatch");
 if(count==0)$fatal(1,"no BIT acceptance after ratio ready");
 end
endtask
initial begin
 repeat(4)@(negedge clk);rst=0;
 configure(32'h006aaaab,32'h00255555);
 configure(32'h53555555,32'h1d2aaaab);
 @(negedge clk);clear=1;#1;
 if(ready)$fatal(1,"clear advertised ready");
 @(negedge clk);clear=0;enable=0;#1;
 if(ready)$fatal(1,"disabled advertised ready");
 @(negedge clk);enable=1;
 repeat(4)@(negedge clk);
 if(!ready||count==0)$fatal(1,"cached ratio restart failed");
 $display("PASS PCM ratio divider, BIT backpressure, clear and enable restart");$finish;
end
initial begin #100000;$fatal(1,"config timeout");end
endmodule
