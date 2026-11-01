`timescale 1ns/1ps
// Bit-exact replay of four 16-byte PCM minor frames at 20 Mbit/s.
// The first 4 bytes are the sync word, followed by SFID and 11 data bytes.
module tb_pcm_major_frame_20m;
reg clk=0, resetn=0, start=0, tvalid=0, tlast=0;
reg [31:0] tdata=0;
reg [3:0] tkeep=0;
wire tready, bit_data, bit_valid, frame_start, frame_end;
wire busy, done, underrun, error;
wire [7:0] error_code;
wire [31:0] bits_sent, frames_completed, bit_phase;
reg [7:0] expected [0:63];
integer frame, byte_index, word_index, accepted=0, failures=0;
integer cycle=0, last_cycle=-1;

always #8.138 clk=~clk; // 61.44 MHz (simulation approximation)
tx_bit_engine_axis #(.AXIS_CLK_HZ(61440000), .BUFFER_WORDS(4096)) dut (
 .axis_clk(clk), .axis_aresetn(resetn), .start_pulse(start),
 .stop_pulse(1'b0), .clear_pulse(1'b0),
 .cfg_bit_count(32'd512), .cfg_repeat_count(32'd1),
 .cfg_bit_rate(32'd20000000), .cfg_rate_word(32'h53555555),
 .s_axis_tdata(tdata), .s_axis_tkeep(tkeep), .s_axis_tvalid(tvalid),
 .s_axis_tready(tready), .s_axis_tlast(tlast),
 .bit_data(bit_data), .bit_valid(bit_valid), .bit_ready(1'b1),
 .frame_start(frame_start), .frame_end(frame_end),
 .busy(busy), .done(done), .underrun(underrun), .error(error),
 .error_code(error_code), .bits_sent(bits_sent),
 .frames_completed(frames_completed), .bit_phase(bit_phase));

always @(posedge clk) begin
 cycle=cycle+1;
 if(bit_valid) begin
  if(bit_data !== expected[(accepted%512)/8][7-(accepted%8)]) begin
   $display("ERROR bit %0d", accepted); failures=failures+1;
  end
  if(frame_start !== (accepted%512==0) ||
     frame_end !== (accepted%512==511)) begin
   $display("ERROR boundary %0d", accepted); failures=failures+1;
  end
  if(last_cycle>=0 && (cycle-last_cycle<3 || cycle-last_cycle>4)) begin
   $display("ERROR rate gap %0d at bit %0d", cycle-last_cycle, accepted);
   failures=failures+1;
  end
  last_cycle=cycle;
  accepted=accepted+1;
 end
end

initial begin
 for(frame=0;frame<4;frame=frame+1) begin
  expected[frame*16+0]=8'hFE;
  expected[frame*16+1]=8'h6B;
  expected[frame*16+2]=8'h28;
  expected[frame*16+3]=8'h40;
  expected[frame*16+4]=frame;
  for(byte_index=0;byte_index<11;byte_index=byte_index+1)
   expected[frame*16+5+byte_index]=frame*11+byte_index;
 end
 repeat(4) @(negedge clk);
 resetn=1;
 @(negedge clk); start=1;
 @(negedge clk); start=0;
 for(word_index=0;word_index<16;word_index=word_index+1) begin
  @(negedge clk);
  tdata={expected[word_index*4+3],expected[word_index*4+2],
         expected[word_index*4+1],expected[word_index*4]};
  tkeep=4'hF; tlast=(word_index==15); tvalid=1;
  do @(posedge clk); while(!tready);
 end
 @(negedge clk); tvalid=0; tlast=0;
 wait(done); @(negedge clk);
 if(accepted!=1024 || frames_completed!=2 || error || underrun)
  failures=failures+1;
 if(failures)
  $fatal(1,"FAIL major-frame 20M: failures=%0d accepted=%0d frames=%0d code=%0d",
         failures,accepted,frames_completed,error_code);
 $display("PASS major-frame 20M: 4 SFIDs, 1024 bits, 2 replays, no bit/rate errors");
 $finish;
end
initial begin #100000; $fatal(1,"major-frame 20M timeout"); end
endmodule
