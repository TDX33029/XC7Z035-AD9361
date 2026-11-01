`timescale 1ns/1ps
module tb_tx_bit_engine_circular;
reg clk=0,resetn=0,start=0,stop=0,clear=0;
reg [31:0] bit_count=16,repeat_count=2,bit_rate=100;
reg [31:0] rate_word=32'hFFFF_FFFF;
reg [31:0] tdata=0;reg [3:0] tkeep=0;reg tvalid=0,tlast=0,bit_ready=1;
wire tready,bit_data,bit_valid,frame_start,frame_end,busy,done,underrun,error;
wire [7:0] error_code;wire [31:0] bits_sent,frames_completed;
wire [31:0] bit_phase;
integer observed=0,failures=0,cycle=0,last_end_cycle=-1;
reg [15:0] pattern=16'hAA55;
/* The two word-boundary bits deliberately differ.  This detects an early
 * BRAM look-ahead that substitutes the next word's bit 31 for the current
 * word's final bit. */
reg [63:0] pattern64=64'hD35AC719_E42B816C;
reg multiword_test=0;
always #5 clk=~clk;
tx_bit_engine_axis #(.AXIS_CLK_HZ(100),.BUFFER_WORDS(4)) dut(
 .axis_clk(clk),.axis_aresetn(resetn),.start_pulse(start),.stop_pulse(stop),.clear_pulse(clear),
 .cfg_bit_count(bit_count),.cfg_repeat_count(repeat_count),.cfg_bit_rate(bit_rate),.cfg_rate_word(rate_word),
 .s_axis_tdata(tdata),.s_axis_tkeep(tkeep),.s_axis_tvalid(tvalid),.s_axis_tready(tready),.s_axis_tlast(tlast),
 .bit_data(bit_data),.bit_valid(bit_valid),.bit_ready(bit_ready),.frame_start(frame_start),.frame_end(frame_end),
 .busy(busy),.done(done),.underrun(underrun),.error(error),.error_code(error_code),
 .bits_sent(bits_sent),.frames_completed(frames_completed),.bit_phase(bit_phase));
always @(posedge clk) begin
 cycle=cycle+1;
 if(bit_valid&&bit_ready)begin
  if(multiword_test)begin
   if(bit_data!==pattern64[63-(observed%64)])begin $display("ERROR multiword data %0d expected=%b actual=%b",observed,pattern64[63-(observed%64)],bit_data);failures=failures+1;end
   if(frame_start!==(observed%64==0))begin $display("ERROR multiword frame_start %0d",observed);failures=failures+1;end
   if(frame_end!==(observed%64==63))begin $display("ERROR multiword frame_end %0d",observed);failures=failures+1;end
  end else begin
   if(bit_data!==pattern[15-(observed%16)])begin $display("ERROR data %0d expected=%b actual=%b",observed,pattern[15-(observed%16)],bit_data);failures=failures+1;end
   if(frame_start!==(observed%16==0))begin $display("ERROR frame_start %0d",observed);failures=failures+1;end
   if(frame_end!==(observed%16==15))begin $display("ERROR frame_end %0d",observed);failures=failures+1;end
  end
  if(frame_start&&last_end_cycle>=0&&cycle-last_end_cycle!=1)begin
   $display("ERROR circular boundary gap=%0d clocks",cycle-last_end_cycle-1);failures=failures+1;end
  if(frame_end)last_end_cycle=cycle;
  observed=observed+1;
 end
end
initial begin
 repeat(4)@(negedge clk);resetn=1;
 @(negedge clk);start=1;@(negedge clk);start=0;
 @(negedge clk);tdata=32'h000055AA;tkeep=4'b0011;tlast=1;tvalid=1;
 while(!tready)@(negedge clk);@(negedge clk);tvalid=0;tlast=0;tkeep=0;
 wait(done);@(negedge clk);
 if(observed!=48||frames_completed!=3||error||underrun)begin
  $display("ERROR final observed=%0d frames=%0d code=%02x",observed,frames_completed,error_code);failures=failures+1;end
 // Infinite replay must remain busy and stop cleanly without a frame gap.
 clear=1;@(negedge clk);clear=0;repeat_count=32'hFFFFFFFF;observed=0;last_end_cycle=-1;
 start=1;@(negedge clk);start=0;@(negedge clk);
 tdata=32'h000055AA;tkeep=4'b0011;tlast=1;tvalid=1;
 while(!tready)@(negedge clk);@(negedge clk);tvalid=0;tlast=0;tkeep=0;
 wait(observed>=64);if(!busy||done||error)begin $display("ERROR infinite status");failures=failures+1;end
 @(negedge clk);stop=1;@(negedge clk);stop=0;@(negedge clk);if(busy)begin $display("ERROR stop");failures=failures+1;end
 // Cross two complete AXI words and verify every last/first bit boundary.
 clear=1;@(negedge clk);clear=0;multiword_test=1;bit_count=64;repeat_count=1;observed=0;last_end_cycle=-1;
 start=1;@(negedge clk);start=0;@(negedge clk);
 tdata=32'h19C75AD3;tkeep=4'b1111;tlast=0;tvalid=1;
 while(!tready)@(negedge clk);@(negedge clk);
 tdata=32'h6C812BE4;tkeep=4'b1111;tlast=1;
 while(!tready)@(negedge clk);@(negedge clk);tvalid=0;tlast=0;tkeep=0;
 wait(done);@(negedge clk);
 if(observed!=128||frames_completed!=2||error||underrun)begin
  $display("ERROR multiword final observed=%0d frames=%0d code=%02x",observed,frames_completed,error_code);failures=failures+1;end
 if(failures==0)$display("PASS: gapless circular replay tests passed");else $display("FAIL: %0d failures",failures);
 $finish;
end
endmodule
