`timescale 1ns/1ps
module tb_soqpsk_irig_audit;
reg clk=0,rst=1,enable=0,clear=0,force_valid=0,scan=0;
reg [31:0] rate_word=32'h14d55555,phase=0;
reg [8:0] pn=9'h1ff;
wire [32:0] sum={1'b0,phase}+{1'b0,rate_word};
wire valid=force_valid || (enable&&!rst&&sum[32]);
wire raw=pn[8];
wire randomized, rv, ready;
wire [11:0] di,dq,oldi,oldq;
wire [31:0] count;
integer errors=0,events=0,prev_delta=0,prev1=-1,prev2=-1;
integer rstate=1,expected_rand,expected_delta,a,alpha,j,n,fd,fsine,fpulse,ft;
reg trace_on=0;
integer case_no=0,cycles=0,last=-1,dt,lo,hi;
reg capture=0;
always #5 if(!scan) clk=~clk;
irig_randomizer rnd(.clk(clk),.rst(rst),.clear(clear),.enable(1'b1),
 .in_data(raw),.in_valid(valid),.out_ready(ready),.out_data(randomized),.out_valid(rv));
soqpsk_tg_modulator dut(.clk(clk),.rst(rst),.enable(enable),.clear(clear),
 .differential_enable(1'b1),.bit_data(randomized),.bit_valid(rv),.bit_ready(ready),
 .bit_phase(phase),.bit_rate_word(rate_word),.frequency_offset_word(32'd0),
 .amplitude(16'd26214),.data_i(di),.data_q(dq),.accepted_bits(count));
soqpsk_tg_baseline old(.clk(clk),.rst(rst),.enable(enable),.clear(clear),
 .differential_enable(1'b1),.bit_data(randomized),.bit_valid(rv),
 .bit_phase(phase),.bit_rate_word(rate_word),.frequency_offset_word(32'd0),
 .amplitude(16'd26214),.data_i(oldi),.data_q(oldq));
always @(posedge clk) begin
 if(!scan) begin
  cycles=cycles+1;
  if(rst||clear||!enable) begin
   phase<=0;pn<=9'h1ff;events=0;prev_delta=0;prev1=-1;prev2=-1;rstate=1;last=-1;
  end else begin
   phase<=sum[31:0];
   if(valid&&ready) begin
    expected_rand=raw ^ ((rstate>>13)&1) ^ ((rstate>>14)&1);
    expected_delta=expected_rand ^ prev_delta ^ ((events%2)==0);
    a=2*expected_delta-1;
    alpha=((events%2)==0 ? -1:1)*prev1*(a-prev2)/2;
    if(randomized!==expected_rand[0] || dut.encoded_bit!==expected_delta[0] || $signed(dut.alpha_next)!=alpha)
     $fatal(1,"IRIG encoder mismatch event=%0d",events);
    if(last>=0) begin
     dt=cycles-last;
     if(dt<lo || dt>hi)$fatal(1,"rate interval %0d outside %0d..%0d",dt,lo,hi);
    end
    last=cycles;rstate=((rstate<<1)|expected_rand)&32767;
    prev_delta=expected_delta;prev2=prev1;prev1=a;events=events+1;
    pn<={pn[7:0],pn[8]^pn[4]};
   end
  end
  if(capture) $fwrite(fd,"%0d %0d %0d %0d\n",$signed(di),$signed(dq),$signed(oldi),$signed(oldq));
  if(trace_on) $fwrite(ft,"%0d %0d %0d %0d %0d %0d\n",phase,valid,randomized,
    dut.phase_acc,dut.shaped_sum_pipe,dut.phase_product);
 end
end
task run_rate;
 input [31:0] word_value;
 input integer lower,upper,number;
 begin
  @(negedge clk);clear=1;capture=0;rate_word=word_value;lo=lower;hi=upper;
  @(negedge clk);clear=0;
  if(number==5000000)begin ft=$fopen("phase_trace.txt","w");trace_on=1;end
  wait(events>=64);@(negedge clk);
  fd=$fopen($sformatf("soqpsk_compare_%0d.txt",number),"w");capture=1;
  repeat(65536)@(negedge clk);
  capture=0;$fclose(fd);
  if(trace_on)begin trace_on=0;$fclose(ft);end
  $display("PASS IRIG encoder/rate case=%0d bits=%0d",number,events);
 end
endtask
initial begin
 repeat(4)@(negedge clk);rst=0;enable=1;
 run_rate(32'd699051,6143,6144,10000);
 run_rate(32'h14d55555,12,13,5000000);
 run_rate(32'h32000000,5,6,12000000);
 run_rate(32'h34155555,4,5,12500000);
 run_rate(32'h40000000,4,4,15360000);
 @(negedge clk);clear=1;force_valid=1;#1;
 if(ready!==0)$fatal(1,"clear advertised a discarded handshake");
 @(negedge clk);clear=0;force_valid=0;
 if(count!=0)$fatal(1,"clear did not reset encoder");
 $display("PASS clear suppresses BIT acceptance");
 // Scan the actual RTL functions independently of stream stimulus.
 scan=1;
 for(j=0;j<8;j=j+1)begin dut.alpha_history[j]=0;old.alpha_history[j]=0;end
 dut.alpha_history[0]=1;old.alpha_history[0]=1;
 fpulse=$fopen("pulse_scan.txt","w");
 for(n=0;n<32768;n=n+1)begin
  for(j=0;j<8;j=j+1)begin dut.alpha_history[j]=(j==n/4096);old.alpha_history[j]=(j==n/4096);end
  phase=(n%4096)<<20;#1;
  $fwrite(fpulse,"%0d %0d %0d\n",n,
   $signed(dut.shaped_sum)+((($signed(dut.shaped_next)-$signed(dut.shaped_sum))*$signed({1'b0,phase[27:20]})+128)>>>8),$signed(old.shaped_sum));
 end
 $fclose(fpulse);
 fsine=$fopen("sine_scan.txt","w");
 for(n=0;n<65536;n=n+1)
  $fwrite(fsine,"%0d %0d %0d\n",n,$signed(dut.sine_interpolated(n[15:0])),$signed(old.sine_lut(n[15:8])));
 $fclose(fsine);
 $display("PASS independent pulse/sine scans generated");
 $finish;
end
initial begin #20000000;$fatal(1,"audit timeout");end
endmodule
