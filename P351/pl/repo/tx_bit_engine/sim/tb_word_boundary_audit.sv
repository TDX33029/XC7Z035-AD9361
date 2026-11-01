`timescale 1ns/1ps
module tb_word_boundary_audit;
reg clk=0,rstn=0,start=0,clear=0,tvalid=0,tlast=0;
reg [31:0] repeat_count=0,tdata=0;
wire tr,bd,bv,fs,fe,busy,done,underrun,error;
wire [7:0] ec;
wire [31:0] sent,frames,phase;
integer cycle=0,last_cycle=-1,count=0,spacing_errors=0,test_mode;
always #5 clk=~clk;
tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000),.BUFFER_WORDS(4)) dut(
 .axis_clk(clk),.axis_aresetn(rstn),.start_pulse(start),.stop_pulse(1'b0),.clear_pulse(clear),
 .cfg_bit_count(32'd64),.cfg_repeat_count(repeat_count),.cfg_bit_rate(32'd15360000),.cfg_rate_word(32'h40000000),
 .s_axis_tdata(tdata),.s_axis_tkeep(4'hf),.s_axis_tvalid(tvalid),.s_axis_tready(tr),.s_axis_tlast(tlast),
 .bit_data(bd),.bit_valid(bv),.bit_ready(1'b1),.frame_start(fs),.frame_end(fe),.busy(busy),.done(done),
 .underrun(underrun),.error(error),.error_code(ec),.bits_sent(sent),.frames_completed(frames),.bit_phase(phase));
always @(posedge clk) begin
 cycle=cycle+1;
 if(bv) begin
   if(last_cycle>=0 && cycle-last_cycle!=4) begin
     spacing_errors=spacing_errors+1;
     $display("AUDIT word_boundary mode=%0d bit=%0d interval=%0d expected=4",test_mode,count,cycle-last_cycle);
   end
   last_cycle=cycle; count=count+1;
 end
end
initial begin
 repeat(4) @(negedge clk); rstn=1;
 for(test_mode=0;test_mode<2;test_mode=test_mode+1) begin
   clear=1; @(negedge clk); clear=0;
   count=0; last_cycle=-1; spacing_errors=0;
   repeat_count=test_mode; start=1; @(negedge clk); start=0;
   tdata=32'h19c75ad3; tvalid=1; tlast=0;
   do @(posedge clk); while(!tr);
   @(negedge clk); tdata=32'h6c812be4; tlast=1;
   do @(posedge clk); while(!tr);
   @(negedge clk); tvalid=0; tlast=0;
   wait(done); @(negedge clk);
   if(spacing_errors || error || underrun || count!=(test_mode==0?64:128)) $fatal(1,"word boundary regression failed");
   $display("AUDIT word_boundary_done mode=%0d accepted=%0d spacing_errors=%0d engine_error=%0d underrun=%0d",test_mode,count,spacing_errors,error,underrun);
 end
 $finish;
end
initial begin #100000; $fatal(1,"timeout"); end
endmodule
