`timescale 1ns/1ps
module tb_stream_prefetch;
reg clk=0,rstn=0,start=0,clear=0,tvalid=0,tlast=0,ready=1;
reg [31:0] count_cfg=257,rate=5000000,qword=32'h14d55555,tdata=0;
reg [3:0] keep=15;
wire tr,bd,bv,fs,fe,busy,done,underrun,error;
wire [7:0] ec;
wire [31:0] sent,frames,phase;
integer cycle=0,observed=0,origin=0,mode,word_idx,stall=0;
reg held=0,held_bit;
reg [63:0] expected_tick;
always #5 clk=~clk;
function [7:0] byte_value(input integer n);
 byte_value=(n*73)^(n>>1)^8'ha5;
endfunction
function [31:0] word_value(input integer n);
 word_value={byte_value(4*n+3),byte_value(4*n+2),byte_value(4*n+1),byte_value(4*n)};
endfunction
tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000),.BUFFER_WORDS(4)) dut(
 .axis_clk(clk),.axis_aresetn(rstn),.start_pulse(start),.stop_pulse(1'b0),.clear_pulse(clear),
 .cfg_bit_count(count_cfg),.cfg_repeat_count(32'd0),.cfg_bit_rate(rate),.cfg_rate_word(qword),
 .s_axis_tdata(tdata),.s_axis_tkeep(keep),.s_axis_tvalid(tvalid),.s_axis_tready(tr),.s_axis_tlast(tlast),
 .bit_data(bd),.bit_valid(bv),.bit_ready(ready),.frame_start(fs),.frame_end(fe),.busy(busy),.done(done),
 .underrun(underrun),.error(error),.error_code(ec),.bits_sent(sent),.frames_completed(frames),.bit_phase(phase));
always @(negedge clk) begin
 if(stall && busy) ready=(cycle%19>4); else ready=1;
end
always @(posedge clk) begin
 cycle=cycle+1;
 if(!rstn || clear || start) held=0;
 else begin
  if(held && (!bv || bd!==held_bit)) $fatal(1,"backpressure changed pending BIT");
  held=bv&&!ready; held_bit=bd;
  if(bv&&ready) begin
   if(bd!==((byte_value(observed/8)>>(7-observed%8))&1'b1)) $fatal(1,"BIT order at %0d",observed);
   if(!stall) begin
    expected_tick=((observed+64'd1)*64'h100000000+qword-1)/qword;
    if(observed==0) origin=cycle-expected_tick;
    if(cycle-origin!=expected_tick) $fatal(1,"fractional timeline at bit %0d",observed);
   end
   observed=observed+1;
  end
 end
end
task begin_task;
begin
 @(negedge clk);clear=1;tvalid=0;repeat(2)@(negedge clk);clear=0;
 observed=0;start=1;@(negedge clk);start=0;
end endtask
task beat(input integer index,input reg last,input [3:0] k);
begin
 @(negedge clk);tdata=word_value(index);tlast=last;keep=k;tvalid=1;
 do @(posedge clk);while(!tr);
 @(negedge clk);tvalid=0;
end endtask
initial begin
 repeat(4)@(negedge clk);rstn=1;
 for(mode=0;mode<4;mode=mode+1) begin
  rate=mode==0?5000000:mode==1?12500000:15360000;
  qword=mode==0?32'h14d55555:mode==1?32'h34155555:32'h40000000;
  stall=mode==3; count_cfg=257;
  begin_task();
  for(word_idx=0;word_idx<9;word_idx=word_idx+1) begin
   repeat(word_idx%3)@(negedge clk);
   beat(word_idx,word_idx==8,word_idx==8?4'h1:4'hf);
  end
  wait(done||error);@(negedge clk);
  if(error||underrun||observed!=257) $fatal(1,"prefetch case mode=%0d sent=%0d err=%0d",mode,observed,ec);
  $display("PASS prefetch rate=%0d bits=257 stall=%0d",rate,stall);
 end
 // Next beat arrives exactly when the last BIT of the current word is accepted.
 stall=0;count_cfg=64;begin_task();beat(0,0,15);
 while(!(bv&&sent==31))@(negedge clk);
 tdata=word_value(1);keep=15;tlast=1;tvalid=1;
 @(posedge clk);if(!tr)$fatal(1,"bypass not ready");
 @(negedge clk);tvalid=0;wait(done||error);@(negedge clk);
 if(error||observed!=64)$fatal(1,"same-cycle handoff failed");
 $display("PASS prefetch same-cycle input/output handoff");
 // Missing next word must report a gap rather than silently re-pace.
 begin_task();beat(0,0,15);wait(error);@(negedge clk);
 if(ec!=5||!underrun||busy||observed!=32)$fatal(1,"missing-data status");
 $display("PASS prefetch missing-data underrun");
 $finish;
end
initial begin #1000000;$fatal(1,"timeout");end
endmodule
