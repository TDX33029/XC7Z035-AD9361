`timescale 1ns/1ps
// Real PS-generated PN11 frame -> production BIT engine -> DDS IRIG15 -> PCM/FM.
// Raw/recovered bits are checked independently, including cached frame replay.
module tb_pcm_rnrz_4m_8m;
 reg clk=0,rst=1,start=0,tvalid=0,tlast=0,recording=0;
 reg [31:0] rate=4000000,word=0,dev=0,tdata=0;
 reg [31:0] payload[0:127];
 wire ready,bd,bv,br,underrun,error,iqvalid,dvalid;
 wire [31:0] phase,sent,frames,accepted;
 wire [11:0] di,dq;
 integer input_file,output_file,events=0,cycles=0,last=-1,lo,hi,j,raw_want,rand_want,decoded;
 reg [14:0] ref_state=15'd1,decoder_state=15'd0;
 reg [14:0] saved_state;
 always #8.138020833333 clk=~clk;
 tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000),.BUFFER_WORDS(128)) engine(
  .axis_clk(clk),.axis_aresetn(!rst),.start_pulse(start),.stop_pulse(1'b0),.clear_pulse(1'b0),
  .cfg_bit_count(32'd4096),.cfg_repeat_count(32'hffffffff),.cfg_bit_rate(rate),.cfg_rate_word(word),
  .s_axis_tdata(tdata),.s_axis_tkeep(4'hf),.s_axis_tvalid(tvalid),.s_axis_tready(ready),.s_axis_tlast(tlast),
  .bit_data(bd),.bit_valid(bv),.bit_ready(br),.bit_phase(phase),.bits_sent(sent),.frames_completed(frames),
  .underrun(underrun),.error(error));
 dds_inc wave(.clk(clk),.rst(rst),.phase_inc(32'd4),.pcm_control(32'h69),
  .bit_data(bd),.bit_valid(bv),.bit_phase(phase),.bit_ready(br),
  .frame_start(1'b0),.frame_end(1'b0),.pcm_deviation_word(dev),.pcm_frequency_offset(32'd0),
  .pcm_amplitude(16'd26214),.pcm_filter_profile(4'd0),.soqpsk_bit_rate_word(word),
  .artm_bit_rate_word(32'd0),.pcm_iq_valid(iqvalid),.pcm_accepted_bits(accepted),
  .dac_valid(dvalid),.dac_data_i0(di),.dac_data_q0(dq));
 always @(posedge clk) begin
  if(rst)begin events=0;cycles=0;last=-1;ref_state=1;decoder_state=0;end
  else if(recording)begin
   $fwrite(input_file,"%0d %0d %0d %0d\n",phase,bv&&br,bd,wave.randomized_bit_data);
   $fwrite(output_file,"%0d %0d %0d %0d\n",wave.pcm_fm_source.phase_acc,$signed(di),$signed(dq),
             $signed(wave.pcm_fm_source.shaped_sum_pipe));
   cycles=cycles+1;
   if(bv&&br)begin
    raw_want=payload[(events%4096)/32][((events%32)/8)*8+7-events%8];
    if(bd!==raw_want[0])$fatal(1,"PS -> BIT ordering/replay mismatch at %0d",events);
    rand_want=raw_want^ref_state[13]^ref_state[14];
    if(wave.randomized_bit_data!==rand_want[0])$fatal(1,"IRIG15 mismatch at %0d",events);
    decoded=wave.randomized_bit_data^decoder_state[13]^decoder_state[14];
    if(events>=15&&decoded!=raw_want)$fatal(1,"self-sync derandomizer mismatch");
    decoder_state={decoder_state[13:0],wave.randomized_bit_data};
    ref_state={ref_state[13:0],rand_want[0]};
    if(last>=0&&(cycles-last<lo||cycles-last>hi))$fatal(1,"cadence gap");
    last=cycles;events=events+1;
   end
   if(underrun||error)$fatal(1,"BIT engine fault");
  end
 end
 task run_case(input integer newrate);
  reg [63:0] wide;
  begin
   @(negedge clk);rst=1;start=0;tvalid=0;tlast=0;recording=0;rate=newrate;
   wide=(64'd4294967296*newrate+30720000)/61440000;word=wide[31:0];
   dev=(wide*35+50)/100;lo=61440000/newrate;hi=(61440000+newrate-1)/newrate;
   repeat(8)@(negedge clk);rst=0;
   repeat(100)@(negedge clk);
   if(!br)$fatal(1,"valid RNRZ-L configuration not ready");
   input_file=$fopen($sformatf("input_%0d_0.txt",rate),"w");
   output_file=$fopen($sformatf("pcm6_%0d_0.txt",rate),"w");
   if(!input_file||!output_file)$fatal(1,"output file unavailable");
   recording=1;start=1;@(negedge clk);start=0;
   for(j=0;j<128;j=j+1)begin
    tvalid=1;tdata=payload[j];tlast=(j==127);
    do @(posedge clk);while(!ready);
    @(negedge clk);
   end
   tvalid=0;tlast=0;
   repeat(524288)@(negedge clk);
   recording=0;$fclose(input_file);$fclose(output_file);
   if(accepted!=events||events<32768||frames<8)$fatal(1,"insufficient acceptance/replay %d %d",events,frames);
   $display("PASS RNRZ-L rate=%0d accepted=%0d frames=%0d samples=%0d: PN11, full-frame IRIG15, self-sync decode, no gaps",
       rate,events,frames,cycles);
  end
 endtask
 initial begin
  $readmemh("C:/Users/Jump/Desktop/AD9361TX/output/pcm_rnrz_audit_20261005/frame_words.hex",payload);
  run_case(4000000);run_case(8000000);$finish;
 end
 initial begin #20000000;$fatal(1,"timeout");end
endmodule
