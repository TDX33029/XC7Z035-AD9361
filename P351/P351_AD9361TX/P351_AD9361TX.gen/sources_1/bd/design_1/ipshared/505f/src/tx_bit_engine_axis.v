`timescale 1 ns / 1 ps
module tx_bit_engine_axis #(
 parameter integer AXIS_CLK_HZ=40000000, parameter integer BUFFER_WORDS=4096
)(
 input wire axis_clk,input wire axis_aresetn,input wire start_pulse,input wire stop_pulse,input wire clear_pulse,
 input wire [31:0] cfg_bit_count,input wire [31:0] cfg_repeat_count,input wire [31:0] cfg_bit_rate,
 input wire [31:0] cfg_rate_word,
 input wire [31:0] s_axis_tdata,input wire [3:0] s_axis_tkeep,input wire s_axis_tvalid,
 output wire s_axis_tready,input wire s_axis_tlast,output wire bit_data,output wire bit_valid,
 input wire bit_ready,output wire frame_start,output wire frame_end,output reg busy,output reg done,
 output reg underrun,output reg error,output reg [7:0] error_code,output reg [31:0] bits_sent,
 output reg [31:0] frames_completed,output wire [31:0] bit_phase
);
localparam [7:0] ERR_NONE=0,ERR_ZERO_COUNT=1,ERR_ZERO_TKEEP=2,ERR_EARLY_TLAST=3,
 ERR_MISSING_TLAST=4,ERR_STREAM_GAP=5,ERR_ZERO_RATE=6,ERR_RATE_HIGH=7,
 ERR_BUFFER_OVERFLOW=8,ERR_BAD_TKEEP=9;
localparam [31:0] AXIS_CLOCK_RATE=AXIS_CLK_HZ;
localparam integer ADDR_WIDTH=(BUFFER_WORDS<=2)?1:$clog2(BUFFER_WORDS);
reg [31:0] active_bit_count,active_bit_rate,active_rate_word,rate_accum;
reg rate_pending;
reg [31:0] word_data;
reg [3:0] word_keep;
reg word_last,word_loaded,stream_started;
// Single-shot AXI stream: prefetch one word while serializing the current one.
reg [31:0] next_word_data;
reg [3:0] next_word_keep;
reg next_word_last,next_word_loaded;
reg [1:0] lane_index;
reg [2:0] bit_index;
reg circular_mode,circular_loading,circular_playing;
reg [2:0] circular_prefetch;
reg [ADDR_WIDTH:0] load_word_index,expected_words;
reg [ADDR_WIDTH-1:0] play_word_index;
reg [31:0] repeats_remaining;
reg [31:0] replay_read_data;

function [1:0] first_valid_lane; input [3:0] keep; begin
 if(keep[0])first_valid_lane=0;else if(keep[1])first_valid_lane=1;
 else if(keep[2])first_valid_lane=2;else first_valid_lane=3; end endfunction
function has_later_lane; input [3:0] keep;input [1:0] lane;begin case(lane)
 0:has_later_lane=|keep[3:1];1:has_later_lane=|keep[3:2];
 2:has_later_lane=keep[3];default:has_later_lane=0;endcase end endfunction
function [1:0] next_valid_lane;input [3:0] keep;input [1:0] lane;begin case(lane)
 0:if(keep[1])next_valid_lane=1;else if(keep[2])next_valid_lane=2;else next_valid_lane=3;
 1:if(keep[2])next_valid_lane=2;else next_valid_lane=3;default:next_valid_lane=3;endcase end endfunction
function [3:0] final_keep;input [31:0] count;reg [2:0] bytes;begin
 bytes=(count[4:0]+7)>>3;case(bytes)1:final_keep=4'b0001;2:final_keep=4'b0011;
 3:final_keep=4'b0111;default:final_keep=4'b1111;endcase end endfunction

/* The Xilinx BRAM read port is clocked. Hold valid during the short prefetch
 * window after the final load beat so address 0 is valid before bit 0. */
wire data_present=circular_mode?(circular_playing&&(circular_prefetch==0)):word_loaded;
wire circular_beat_ok = s_axis_tvalid && s_axis_tready && (s_axis_tkeep!=0) &&
  !((load_word_index==expected_words-1) && !s_axis_tlast) &&
  !((load_word_index!=expected_words-1) && s_axis_tlast) &&
  !((load_word_index==expected_words-1 && s_axis_tkeep!=final_keep(active_bit_count)) ||
    (load_word_index!=expected_words-1 && s_axis_tkeep!=4'hF));
assign s_axis_tready=busy&&(circular_mode?(circular_loading&&load_word_index<expected_words):
 (!word_loaded || (!next_word_loaded && !word_last)));
/* Q0.32 scheduler: retain the modulo residue across every accepted bit. */
wire [32:0] rate_sum={1'b0,rate_accum}+{1'b0,active_rate_word};
wire rate_overflow=data_present&&!rate_pending&&active_rate_word!=0&&rate_sum[32];
wire rate_enable=rate_pending||rate_overflow;
assign bit_valid=busy&&data_present&&rate_enable;
assign bit_data=circular_mode?replay_read_data[{lane_index,3'b000}+bit_index]:word_data[{lane_index,3'b000}+bit_index];
assign frame_start=bit_valid&&bits_sent==0;
assign frame_end=bit_valid&&bits_sent==active_bit_count-1;
assign bit_phase=rate_accum;
/* Keep the BRAM on the current word through bit 31.  The previous look-ahead
 * changed the synchronous read address while bit_index==0, which is held for
 * several clocks at fractional bit rates.  Consequently the final bit of a
 * 32-bit word was sometimes taken from the next word.  Advance the address
 * only after that bit is accepted; the normal >=1 clock interval before the
 * next bit is ample for the synchronous BRAM read. */
/* Inferred true dual-port block RAM.  Keeping simulation and synthesis on
 * this single implementation prevents the primitive model from hiding word
 * boundary behavior.  Port A loads complete AXI words and port B performs a
 * synchronous replay read; Vivado maps 4096x32 to block RAM. */
(* ram_style = "block" *) reg [31:0] replay_mem [0:BUFFER_WORDS-1];
always @(posedge axis_clk) begin
 if(!axis_aresetn) replay_read_data <= 0;
 else if(circular_beat_ok)
   replay_mem[load_word_index[ADDR_WIDTH-1:0]] <= s_axis_tdata;
 if(axis_aresetn && circular_mode && (circular_loading || circular_playing))
   replay_read_data <= replay_mem[play_word_index];
end

always @(posedge axis_clk) begin
 if(!axis_aresetn||clear_pulse||stop_pulse)begin active_bit_rate<=0;active_rate_word<=0;rate_accum<=0;rate_pending<=0;end
 else if(start_pulse)begin active_bit_rate<=cfg_bit_rate;active_rate_word<=cfg_rate_word;rate_accum<=0;rate_pending<=0;end
 else if(!busy||!data_present)begin rate_accum<=0;rate_pending<=0;end
 else if(rate_pending)begin if(bit_valid&&bit_ready)rate_pending<=0;end
 else if(rate_overflow)begin rate_accum<=rate_sum[31:0];if(!bit_ready)rate_pending<=1;end
 else rate_accum<=rate_sum[31:0];
end

always @(posedge axis_clk) begin
 if(!axis_aresetn)begin
 active_bit_count<=0;word_data<=0;word_keep<=0;word_last<=0;word_loaded<=0;stream_started<=0;
  next_word_data<=0;next_word_keep<=0;next_word_last<=0;next_word_loaded<=0;
  lane_index<=0;bit_index<=7;busy<=0;done<=0;underrun<=0;error<=0;error_code<=ERR_NONE;
  bits_sent<=0;frames_completed<=0;circular_mode<=0;circular_loading<=0;circular_playing<=0;circular_prefetch<=0;
  load_word_index<=0;expected_words<=0;play_word_index<=0;repeats_remaining<=0;
 end else if(clear_pulse)begin
  next_word_loaded<=0;
  word_loaded<=0;stream_started<=0;busy<=0;done<=0;underrun<=0;error<=0;error_code<=ERR_NONE;
  bits_sent<=0;frames_completed<=0;circular_mode<=0;circular_loading<=0;circular_playing<=0;
 end else if(stop_pulse)begin
  next_word_loaded<=0;
  word_loaded<=0;stream_started<=0;busy<=0;done<=0;circular_loading<=0;circular_playing<=0;circular_prefetch<=0;
 end else if(start_pulse)begin
  next_word_loaded<=0;
  active_bit_count<=cfg_bit_count;word_loaded<=0;stream_started<=0;lane_index<=0;bit_index<=7;
  done<=0;underrun<=0;bits_sent<=0;frames_completed<=0;load_word_index<=0;play_word_index<=0;
  repeats_remaining<=cfg_repeat_count;expected_words<=(cfg_bit_count+31)>>5;
  circular_mode<=cfg_repeat_count!=0;circular_playing<=0;circular_prefetch<=0;
  circular_loading<=cfg_repeat_count!=0&&cfg_bit_count!=0&&((cfg_bit_count+31)>>5)<=BUFFER_WORDS;
  error<=(cfg_bit_count==0)||(cfg_bit_rate==0)||(cfg_rate_word==0)||(cfg_bit_rate>AXIS_CLOCK_RATE)||
         (cfg_repeat_count!=0&&((cfg_bit_count+31)>>5)>BUFFER_WORDS);
  if(cfg_bit_count==0)error_code<=ERR_ZERO_COUNT;else if(cfg_bit_rate==0||cfg_rate_word==0)error_code<=ERR_ZERO_RATE;
  else if(cfg_bit_rate>AXIS_CLOCK_RATE)error_code<=ERR_RATE_HIGH;
  else if(cfg_repeat_count!=0&&((cfg_bit_count+31)>>5)>BUFFER_WORDS)error_code<=ERR_BUFFER_OVERFLOW;
  else error_code<=ERR_NONE;
  busy<=cfg_bit_count!=0&&cfg_bit_rate!=0&&cfg_rate_word!=0&&cfg_bit_rate<=AXIS_CLOCK_RATE&&
        (cfg_repeat_count==0||((cfg_bit_count+31)>>5)<=BUFFER_WORDS);
 end else if(busy&&circular_mode&&circular_loading)begin
  if(s_axis_tvalid&&s_axis_tready)begin
   if(s_axis_tkeep==0)begin busy<=0;underrun<=1;error<=1;error_code<=ERR_ZERO_TKEEP;end
   else if(load_word_index==expected_words-1&&!s_axis_tlast)begin busy<=0;underrun<=1;error<=1;error_code<=ERR_MISSING_TLAST;end
   else if(load_word_index!=expected_words-1&&s_axis_tlast)begin busy<=0;underrun<=1;error<=1;error_code<=ERR_EARLY_TLAST;end
   else if((load_word_index==expected_words-1&&s_axis_tkeep!=final_keep(active_bit_count))||
           (load_word_index!=expected_words-1&&s_axis_tkeep!=4'hF))begin busy<=0;underrun<=1;error<=1;error_code<=ERR_BAD_TKEEP;end
   else begin
    if(load_word_index==expected_words-1)begin circular_loading<=0;circular_playing<=1;circular_prefetch<=5;lane_index<=0;bit_index<=7;
     /* Toggle the BRAM read address once after the final write.  This avoids
      * a read/write same-address collision leaving the first word stale in
      * the primitive model and in silicon. */
     play_word_index<=1;end
    load_word_index<=load_word_index+1;
 end
 end
end else if(busy&&circular_mode&&circular_playing)begin
  if(circular_prefetch!=0) begin
   circular_prefetch<=circular_prefetch-1'b1;
   if(circular_prefetch==2) play_word_index<=0;
  end
  if(bit_valid&&bit_ready)begin
   if(bits_sent==active_bit_count-1)begin
    frames_completed<=frames_completed+1;
    if(repeats_remaining==0)begin bits_sent<=bits_sent+1;busy<=0;done<=1;circular_playing<=0;end
    else begin bits_sent<=0;lane_index<=0;bit_index<=7;play_word_index<=0;
     if(repeats_remaining!=32'hFFFFFFFF)repeats_remaining<=repeats_remaining-1;end
   end else begin
    bits_sent<=bits_sent+1;
    if(bit_index!=0)bit_index<=bit_index-1;
    else if(lane_index!=3)begin lane_index<=lane_index+1;bit_index<=7;end
    else begin lane_index<=0;bit_index<=7;play_word_index<=play_word_index+1;
     end
   end
  end
 end else if(busy)begin
  if(s_axis_tvalid&&s_axis_tready&&s_axis_tkeep==0)begin
   busy<=0;underrun<=1;error<=1;error_code<=ERR_ZERO_TKEEP;
  end else begin
   if(s_axis_tvalid&&s_axis_tready)begin
    if(!word_loaded)begin
     word_data<=s_axis_tdata;word_keep<=s_axis_tkeep;word_last<=s_axis_tlast;word_loaded<=1;
     stream_started<=1;lane_index<=first_valid_lane(s_axis_tkeep);bit_index<=7;
    end else begin
     next_word_data<=s_axis_tdata;next_word_keep<=s_axis_tkeep;
     next_word_last<=s_axis_tlast;next_word_loaded<=1;
    end
   end
   // Input prefetch and BIT consumption are independent, including same-cycle handoff.
   if(bit_valid&&bit_ready)begin
   bits_sent<=bits_sent+1;
   if(bits_sent==active_bit_count-1)begin word_loaded<=0;busy<=0;done<=1;frames_completed<=1;
    if(!word_last)begin error<=1;error_code<=ERR_MISSING_TLAST;end end
   else if(bit_index!=0)bit_index<=bit_index-1;
   else if(has_later_lane(word_keep,lane_index))begin lane_index<=next_valid_lane(word_keep,lane_index);bit_index<=7;end
   else if(word_last)begin busy<=0;underrun<=1;error<=1;error_code<=ERR_EARLY_TLAST;end
   else if(next_word_loaded || (s_axis_tvalid&&s_axis_tready))begin
    word_data<=next_word_loaded?next_word_data:s_axis_tdata;
    word_keep<=next_word_loaded?next_word_keep:s_axis_tkeep;
    word_last<=next_word_loaded?next_word_last:s_axis_tlast;
    lane_index<=first_valid_lane(next_word_loaded?next_word_keep:s_axis_tkeep);
    bit_index<=7;next_word_loaded<=0;
    // Keep word_loaded and rate_accum intact across every AXI word boundary.
   end else begin
    busy<=0;underrun<=1;error<=1;error_code<=ERR_STREAM_GAP;
   end
   end
  end
 end
end
endmodule
