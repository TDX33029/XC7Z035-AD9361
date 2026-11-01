`timescale 1ns/1ps
// Feasibility bench only: production dds_inc range guards are NOT modified.
module rate20_tb;
reg clk=0,rst=1,clear=0,start=0,tvalid=0,tlast=0,recording=0;
reg [31:0] rate=5000000,word=32'h14d55555,dev=0,tdata=0;
reg [31:0] payload[0:1023];
wire ready,bd,bv,br,randomized,rv,underrun,error;
wire [31:0] phase,sent,frames;
integer fd,cycles=0,events=0,last=-1,lo,hi,dt,caseid,j,n,captured,pattern;
integer rn=1,expected_rand;
reg [22:0] pn;
always #8.138 clk=~clk; // rate measurements use nominal 61.44 MHz, not rounded simulator time.
assign br=!rst&&!clear&&pcm[0].dut.bit_ready;
tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000),.BUFFER_WORDS(1024)) engine(
 .axis_clk(clk),.axis_aresetn(!rst),.start_pulse(start),.stop_pulse(1'b0),.clear_pulse(clear),
 .cfg_bit_count(32'd32768),.cfg_repeat_count(32'hffffffff),.cfg_bit_rate(rate),.cfg_rate_word(word),
 .s_axis_tdata(tdata),.s_axis_tkeep(4'hf),.s_axis_tvalid(tvalid),.s_axis_tready(ready),.s_axis_tlast(tlast),
 .bit_data(bd),.bit_valid(bv),.bit_ready(br),.bit_phase(phase),.bits_sent(sent),.frames_completed(frames),
 .underrun(underrun),.error(error));
irig_randomizer randomizer(.clk(clk),.rst(rst),.clear(clear),.enable(1'b1),
 .in_data(bd),.in_valid(bv),.out_ready(br),.out_data(randomized),.out_valid(rv));
genvar g;
generate for(g=0;g<7;g=g+1) begin: pcm
 wire [11:0] di,dq;wire [31:0] accepted;integer f;
 pcm_fm_modulator dut(.clk(clk),.rst(rst),.clear(clear),.enable(1'b1),
 .bit_data(g==6?randomized:bd),.bit_valid(bv),.bit_phase(phase),.bit_rate_word(word),
 .deviation_word(dev),.frequency_offset_word(32'd0),.amplitude(16'd26214),
 .filter_profile(4'd0),.line_code(g),.rolloff(3'd0),.data_i(di),.data_q(dq),.accepted_bits(accepted));
 always @(posedge clk) if(recording)
   $fwrite(f,"%0d %0d %0d %0d\n",dut.phase_acc,$signed(di),$signed(dq),$signed(dut.shaped_sum_pipe));
end endgenerate
wire [11:0] si,sq;wire [31:0] sa;integer sf;
soqpsk_tg_modulator soq(.clk(clk),.rst(rst),.clear(clear),.enable(1'b1),.differential_enable(1'b1),
 .bit_data(randomized),.bit_valid(bv),.bit_phase(phase),.bit_rate_word(word),
 .frequency_offset_word(32'd0),.amplitude(16'd26214),.data_i(si),.data_q(sq),.accepted_bits(sa));
always @(posedge clk) begin
 if(recording) begin
   $fwrite(fd,"%0d %0d %0d %0d\n",phase,bv,bd,randomized);
   $fwrite(sf,"%0d %0d %0d %0d\n",soq.phase_acc,$signed(si),$signed(sq),$signed(soq.shaped_sum_pipe));
   cycles=cycles+1;
 end
 if(clear||rst) begin events=0;last=-1;rn=1;end
 else if(bv&&br) begin
   if(bd!==payload[(events%32768)/32][((events%32)/8)*8+7-events%8]) $fatal(1,"BIT ordering failure %d",events);
   expected_rand=bd^((rn>>13)&1)^((rn>>14)&1);
   if(randomized!==expected_rand[0])$fatal(1,"randomizer failure");
   rn=((rn<<1)|expected_rand)&32767;
   if(last>=0)begin dt=cycles-last;if(dt<lo||dt>hi)$fatal(1,"BIT interval %d",dt);end
   last=cycles;events=events+1;
 end
 if(!rst&&!clear&&(underrun||error))$fatal(1,"engine error");
end
task run_case;
 input integer newrate,newpattern;
 begin
 @(negedge clk);recording=0;clear=1;rate=newrate;pattern=newpattern;
 word=$rtoi((4294967296.0*newrate/61440000.0)+0.5);
 dev=$rtoi((4294967296.0*0.35*newrate/61440000.0)+0.5);
 lo=61440000/newrate;hi=(61440000+newrate-1)/newrate;
 pn=23'h1;
 for(j=0;j<1024;j=j+1)begin
  payload[j]=0;
  for(n=0;n<32;n=n+1)begin
   payload[j][(n/8)*8+7-n%8]=(pattern==0)?pn[22]:((pattern==1)?n%2:(pattern==2?0:1));
   pn={pn[21:0],pn[22]^pn[17]};
  end
 end
 repeat(4)@(negedge clk);clear=0;start=1;
 @(negedge clk);start=0;
 for(j=0;j<1024;j=j+1)begin
   tvalid=1;tdata=payload[j];tlast=(j==1023);
   do @(posedge clk);while(!ready);
   @(negedge clk);
 end
 tvalid=0;tlast=0;cycles=0;
 fd=$fopen($sformatf("input_%0d_%0d.txt",rate,pattern),"w");
 sf=$fopen($sformatf("soq_%0d_%0d.txt",rate,pattern),"w");
 pcm[0].f=$fopen($sformatf("pcm0_%0d_%0d.txt",rate,pattern),"w");
 pcm[1].f=$fopen($sformatf("pcm1_%0d_%0d.txt",rate,pattern),"w");
 pcm[2].f=$fopen($sformatf("pcm2_%0d_%0d.txt",rate,pattern),"w");
 pcm[3].f=$fopen($sformatf("pcm3_%0d_%0d.txt",rate,pattern),"w");
 pcm[4].f=$fopen($sformatf("pcm4_%0d_%0d.txt",rate,pattern),"w");
 pcm[5].f=$fopen($sformatf("pcm5_%0d_%0d.txt",rate,pattern),"w");
 pcm[6].f=$fopen($sformatf("pcm6_%0d_%0d.txt",rate,pattern),"w");
 recording=1;
 repeat(131072)@(negedge clk);
 recording=0;
 $fclose(fd);$fclose(sf);
 $fclose(pcm[0].f);$fclose(pcm[1].f);$fclose(pcm[2].f);$fclose(pcm[3].f);
 $fclose(pcm[4].f);$fclose(pcm[5].f);$fclose(pcm[6].f);
 if(sa!=events||pcm[0].accepted!=events||pcm[1].accepted!=events||pcm[2].accepted!=events||pcm[3].accepted!=events||pcm[4].accepted!=events||pcm[5].accepted!=events||pcm[6].accepted!=events)$fatal(1,"acceptance count mismatch");
 $display("PASS rate=%0d pattern=%0d word=%h accepted=%0d frames=%0d samples=%0d",rate,pattern,word,events,frames,cycles);
 clear=1;@(negedge clk);
 end endtask
initial begin
 repeat(8)@(negedge clk);rst=0;
 run_case(100000,0);run_case(5000000,0);run_case(12500000,0);
 run_case(15360000,0);run_case(18000000,0);run_case(20000000,0);
 run_case(20000000,1);run_case(20000000,2);run_case(20000000,3);
 $display("PASS all feasibility cases");$finish;
end
initial begin #30000000;$fatal(1,"timeout");end
endmodule
