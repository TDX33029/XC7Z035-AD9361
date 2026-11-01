`timescale 1ns/1ps
module tb_irig_randomizer;
reg clk=0, rst=1, clear=0, enable=0, in_data=0, in_valid=0, out_ready=1;
wire out_data, out_valid;
reg [14:0] model_state;
integer i;
always #5 clk=~clk;
irig_randomizer dut(.clk(clk),.rst(rst),.clear(clear),.enable(enable),
 .in_data(in_data),.in_valid(in_valid),.out_ready(out_ready),
 .out_data(out_data),.out_valid(out_valid));
task accept_and_check(input reg value);
 reg expected;
 begin
  in_data=value; in_valid=1;
  expected=value^model_state[13]^model_state[14]; #1;
  if(!out_valid || out_data!==expected) $fatal(1,"randomizer mismatch");
  @(posedge clk); model_state={model_state[13:0],expected}; #1;
 end
endtask
initial begin
 repeat(3) @(posedge clk); rst=0; model_state=15'b1;
 in_valid=1; in_data=1; #1;
 if(out_data!==1 || out_valid!==1) $fatal(1,"bypass mismatch");
 in_valid=0; @(posedge clk); #1; enable=1;
 for(i=0;i<128;i=i+1) accept_and_check((i*7+3)&1);
 out_ready=0; in_data=1; in_valid=1;
 repeat(7) begin #1; if(out_data!==(1^model_state[13]^model_state[14])) $fatal(1,"stall mismatch"); @(posedge clk); end
 out_ready=1; accept_and_check(1);
 clear=1; @(posedge clk); clear=0; model_state=15'b1; accept_and_check(0);
 $display("PASS tb_irig_randomizer"); $finish;
end
endmodule
