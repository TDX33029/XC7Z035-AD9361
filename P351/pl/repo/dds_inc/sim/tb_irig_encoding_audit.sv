`timescale 1ns/1ps
// Independent IRIG-106-24 Fig.4-1 transition-property audit.
// Regression: report all code cases, then fail on any standard-rule violation.
module tb_irig_encoding_audit;
reg clk=0,rst=1,clear=0,enable=1,raw=0,valid=0;
reg [2:0] code=0;
reg [31:0] phase=0;
wire randomized,rv,ready,iqv;
wire [11:0] di,dq;
wire [31:0] accepted,dp;
wire signed [15:0] nrz,filter;
integer c,n,first,second,previous_end,errors,compared,total_errors;
integer random_errors,decoded,ones,period_errors;
reg [31:0] pattern=32'h9e3779b9;
reg [14:0] received=0;
reg [14:0] saved;
reg out_before;
irig_randomizer randomizer(.clk(clk),.rst(rst),.clear(clear),.enable(code==6),
 .in_data(raw),.in_valid(valid),.out_ready(ready),.out_data(randomized),.out_valid(rv));
pcm_fm_modulator dut(.clk(clk),.rst(rst),.clear(clear),.enable(enable),
 .bit_data(randomized),.bit_valid(rv),.bit_ready(ready),.bit_phase(phase),
 .bit_rate_word(32'h14d55555),.deviation_word(32'h074aaaab),
 .frequency_offset_word(32'd0),.amplitude(16'd26214),.filter_profile(4'd0),
 .line_code(code),.rolloff(3'd0),.data_i(di),.data_q(dq),.iq_valid(iqv),
 .accepted_bits(accepted),.debug_nrz(nrz),.debug_filter(filter),.debug_phase(dp));
always #5 clk=~clk;
task reset_code(input integer mode);
begin
 @(negedge clk); code=mode; clear=1; valid=0; phase=0;
 repeat(3) @(negedge clk); clear=0;
 wait(ready); @(negedge clk);
 previous_end=-1; received=0; errors=0; compared=0; random_errors=0;
end endtask
task send(input reg b);
begin
 @(negedge clk); raw=b; valid=1; phase=0;
 @(posedge clk); #1;
 @(negedge clk); valid=0; phase=32'h40000000;
 @(posedge clk); #1; first=(nrz>0);
 @(negedge clk); phase=32'hc0000000;
 @(posedge clk); #1; second=(nrz>0);
 // Ignore only the unknown initial polarity for differential conventions.
 if(code==0 && (first!=b || second!=b)) errors=errors+1;
 if(code==1 && (first!=second || (previous_end>=0 && ((first!=previous_end)!=b)))) errors=errors+1;
 if(code==2 && (first!=second || (previous_end>=0 && ((first!=previous_end)!=(!b))))) errors=errors+1;
 if(code==3 && (first!=b || second==b)) errors=errors+1;
 // IRIG Bi-M: midpoint always transitions; start transitions on ZERO.
 if(code==4 && (first==second || (previous_end>=0 && ((first!=previous_end)!=(!b))))) errors=errors+1;
 // IRIG Bi-S: midpoint always transitions; start transitions on ONE.
 if(code==5 && (first==second || (previous_end>=0 && ((first!=previous_end)!=b)))) errors=errors+1;
 if(code==6) begin
   if(first!=second) errors=errors+1;
   decoded=first ^ received[13] ^ received[14];
   if(compared>=15 && decoded!=b) random_errors=random_errors+1;
   received={received[13:0],first[0]};
 end
 previous_end=second; compared=compared+1;
end endtask
initial begin
 total_errors=0;
 repeat(4) @(negedge clk); rst=0;
 for(c=0;c<7;c=c+1) begin
   reset_code(c);
   for(n=0;n<4096;n=n+1) begin
     pattern=pattern^(pattern<<13); pattern=pattern^(pattern>>17); pattern=pattern^(pattern<<5);
     // Long constant runs plus mixed data, crossing many conceptual frames.
     send(n<256 ? 1'b0 : n<512 ? 1'b1 : pattern[0]);
   end
   $display("AUDIT code=%0d bits=%0d transition_errors=%0d derandomizer_errors=%0d accepted=%0d",c,compared,errors,random_errors,accepted);
   total_errors=total_errors+errors+random_errors;
 end
 reset_code(6); ones=0; period_errors=0;
 for(n=0;n<32767;n=n+1) begin
   send(0); ones=ones+first;
   if(n<32766 && randomizer.state==15'd1) period_errors=period_errors+1;
 end
 if(randomizer.state!=15'd1 || ones!=16384) period_errors=period_errors+1;
 $display("AUDIT RNRZ zero-input period=32767 ones=%0d period_errors=%0d decode_errors=%0d",ones,period_errors,random_errors);
 // Force pending valid across clear. Both DUT and upstream randomizer must
 // reject it, then accept exactly the first post-clear bit on release.
 @(negedge clk); clear=1; valid=1; raw=1; #1;
 if(ready!==0 || iqv!==0) $fatal(1,"PCM clear advertised discarded data");
 repeat(3) @(negedge clk);
 if(accepted!==0 || randomizer.state!==15'd1)
   $fatal(1,"PCM clear failed to reset stream state");
 clear=0;
 @(posedge clk); #1;
 // debug_nrz is registered one cycle after chip_first/second.
 if(accepted!==1 || randomizer.state!==15'd3 || dut.chip_first!==1'b1)
   $fatal(1,"PCM first post-clear BIT lost or randomized incorrectly");
 @(negedge clk); valid=0;
 $display("PASS PCM clear blocks handshake and preserves first released BIT");
 // Backpressure: randomizer must not consume any unaccepted bit.
 @(negedge clk); enable=0; valid=1; raw=1; saved=randomizer.state;
 repeat(20) @(negedge clk);
 $display("AUDIT randomizer_stall_pass=%0d",randomizer.state==saved);
 $display("AUDIT FINISHED total_line_errors=%0d",total_errors);
if(total_errors || period_errors || random_errors || randomizer.state!=saved)
$fatal(1,"IRIG encoding regression failed");
 $finish;
end
endmodule
