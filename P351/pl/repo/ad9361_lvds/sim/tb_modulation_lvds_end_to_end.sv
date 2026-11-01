`timescale 1ns/1ps
// Production BIT engine -> DDS (randomizer/modulator/mux) -> LVDS DDR pins.
// Numerical modulation conformity is checked separately against analytic
// IRIG pulses; here every pin-reconstructed I/Q sample must be bit-exact.
module tb_modulation_lvds_end_to_end;
 reg dc=0,sc=0,rst=1,locked=0,start=0,tvalid=0,tlast=0;
 reg [31:0] mode=0,control=0,rate=5000000,word=32'h14d55555,dev=0,tdata=0;
 reg [31:0] payload[0:15];
 wire ready,bd,bv,br,fs,fe,underrun,error,dvalid,iqvalid;
 wire [31:0] phase,sent,frames,accepted;
 wire [11:0] di,dq;
 wire cp,cn,fp,fn;wire [5:0] dp,dn;
 always #4.069010416667 dc=~dc;
 initial forever begin repeat(2) @(dc);#0.7;sc=~sc;end
 tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000),.BUFFER_WORDS(16)) engine(
  .axis_clk(sc),.axis_aresetn(!rst),.start_pulse(start),.stop_pulse(1'b0),.clear_pulse(1'b0),
  .cfg_bit_count(32'd512),.cfg_repeat_count(32'hffffffff),.cfg_bit_rate(rate),.cfg_rate_word(word),
  .s_axis_tdata(tdata),.s_axis_tkeep(4'hf),.s_axis_tvalid(tvalid),.s_axis_tready(ready),.s_axis_tlast(tlast),
  .bit_data(bd),.bit_valid(bv),.bit_ready(br),.bit_phase(phase),.frame_start(fs),.frame_end(fe),
  .bits_sent(sent),.frames_completed(frames),.underrun(underrun),.error(error));
 dds_inc wave(.clk(sc),.rst(rst),.phase_inc(mode),.bit_data(bd),.bit_valid(bv),.bit_phase(phase),
  .frame_start(fs),.frame_end(fe),.bit_ready(br),.pcm_deviation_word(dev),.pcm_frequency_offset(32'd0),
  .pcm_amplitude(16'd26214),.pcm_filter_profile(4'd0),.pcm_control(control),
  .soqpsk_bit_rate_word(word),.artm_bit_rate_word(32'd0),.pcm_iq_valid(iqvalid),
  .pcm_accepted_bits(accepted),.dac_valid(dvalid),.dac_data_i0(di),.dac_data_q0(dq));
 ad9361_lvds_tx phy(.rst(rst),.data_clk(dc),.sample_clk(sc),.sample_locked(locked),
  .dac_valid(dvalid),.dac_data_i0(di),.dac_data_q0(dq),.dac_data_i1(12'd0),.dac_data_q1(12'd0),
  .tx_clk_out_p(cp),.tx_clk_out_n(cn),.tx_frame_out_p(fp),.tx_frame_out_n(fn),
  .tx_data_out_p(dp),.tx_data_out_n(dn));
 reg [23:0] expected[0:65535];
 integer wr=0,rd=0,slot=0,total=0,events=0,nonzero=0,j,n,k;
 reg aligned=0,previous_frame=0;reg [11:0] ri,rq;reg [10:0] pn;
 reg [31:0] counter=0,last_event=0;integer min_dt,max_dt;
 always @(posedge sc) begin
  if(rst||!locked) begin wr=0;events=0;counter=0;last_event=0;end
  else begin
   counter=counter+1;
   if(phy.sample_ready&&dvalid&&!phy.queue_full) begin
    if(wr>=65536) $fatal(1,"scoreboard capacity");
    expected[wr]={di,dq};wr=wr+1;
   end
   if(bv&&br) begin
    if(bd!==payload[(events%512)/32][((events%32)/8)*8+7-events%8])
      $fatal(1,"MSB-first data mismatch mode=%d event=%d",mode,events);
    if(events>8&&(counter-last_event<min_dt||counter-last_event>max_dt))
      $fatal(1,"sample cadence mismatch mode=%d dt=%d",mode,counter-last_event);
    events=events+1;last_event=counter;
   end
   if(underrun||error) $fatal(1,"BIT engine fault");
  end
 end
 initial forever begin
  @(posedge dc);#0.2;
  if(rst||!locked) begin aligned=0;previous_frame=0;rd=0;total=0;nonzero=0;end
  else begin
   if(!previous_frame&&fp&&!aligned) begin aligned=1;slot=0;end
   if(aligned) begin
    if(fp!==(slot==0)) $fatal(1,"1R1T FRAME cadence");
    case(slot) 0:ri[11:6]=dp;1:ri[5:0]=dp;endcase
   end
   @(negedge dc);#0.2;
   if(aligned) begin
    case(slot) 0:rq[11:6]=dp;1:rq[5:0]=dp;endcase
    if(slot==1) begin
     if(rd>=wr||{ri,rq}!==expected[rd])
       $fatal(1,"LVDS sample mismatch mode=%d sample=%d got=%h want=%h",mode,rd,{ri,rq},expected[rd]);
     if(cp!==1'b1||cn!==1'b0||fn!==~fp||dn!==~dp) $fatal(1,"differential pin mismatch");
     if(phy.overflow_sticky||phy.underflow_sticky) $fatal(1,"LVDS FIFO fault");
     if({ri,rq}!=0) nonzero=nonzero+1;
     rd=rd+1;total=total+1;
    end
    slot=(slot+1)%2;
   end
   previous_frame=fp;
  end
 end
 task run_case(input integer newmode,newrate);
  reg [63:0] wide;
  begin
   @(negedge sc);rst=1;locked=0;start=0;tvalid=0;tlast=0;
   mode=newmode;control=newmode==4?32'h69:32'h0d;rate=newrate;
   wide=(64'd4294967296*newrate+30720000)/61440000;word=wide[31:0];
   wide=(wide*35+50)/100;dev=wide[31:0];
   min_dt=61440000/newrate;max_dt=(61440000+newrate-1)/newrate;
   pn=11'h7ff;
   for(j=0;j<16;j=j+1) begin
    payload[j]=0;
    for(n=0;n<32;n=n+1) begin
     payload[j][(n/8)*8+7-n%8]=pn[0];pn={pn[0]^pn[2],pn[10:1]};
    end
   end
   repeat(10) @(negedge sc);rst=0;locked=1;
   repeat(96) @(negedge sc);start=1;@(negedge sc);start=0;
   for(j=0;j<16;j=j+1) begin
    tvalid=1;tdata=payload[j];tlast=j==15;
    do @(posedge sc);while(!ready);
    @(negedge sc);
   end
   tvalid=0;tlast=0;wait(total>=20000);@(negedge sc);
   if(accepted!=events||nonzero<16000) $fatal(1,"inactive/mismatched modulation mode=%d",mode);
   $display("PASS modulation LVDS mode=%d rate=%d pin_samples=%d bits=%d frames=%d",mode,rate,total,events,frames);
  end
 endtask
 initial begin
  run_case(4,100000);run_case(3,100000);
  run_case(4,5000000);run_case(3,5000000);
  run_case(4,20000000);run_case(3,20000000);
  run_case(4,4000000);run_case(4,8000000);
  $display("PASS full modulation -> 1R1T LVDS pins, eight mode/rate cases");$finish;
 end
 initial begin #4000000;$fatal(1,"timeout");end
endmodule
