`timescale 1 ns / 1 ps

// PCM/FM complex-baseband transmitter for the AD9361 direct-I/Q interface.
//
// The input bit stream is already rate paced by tx_bit_engine in the
// 61.44 MHz sample-clock domain.  Each accepted NRZ-L symbol excites the
// IRIG-106 fourth-order Bessel response (Chapter 2, Table 2-3), with
// -3 dB corner 0.7*Rb. Phase generation integrates half-bit chip responses:
// Q24 area, 32 nodes/Tb, 12-bit interpolation and a 4-Tb numerical tail.
// Exact half-area endpoints and completed-tail accounting prevent phase
// drift from rectangular frequency integration at fractional samples/bit.
// The original Q15 frequency convolution is retained for ILA diagnostics;
// it is no longer the source of the transmitted phase.
//
// filter_profile remains in the interface for backward-compatible PS/UI
// packets.  Pulse shape is rate-normalized and therefore independent of Rb.
// bit_phase is the shared Q0.32 symbol phase from tx_bit_engine.  Keeping the
// fractional residue at each symbol boundary is required for non-integer
// samples/bit rates such as 4.2 and 12.5 Mbit/s.  The common PCM/SOQPSK
// project ceiling is 20 Mbit/s (3.072 complex samples per BIT).
// Configuration of deviation/Rb takes 64 clocks with bit_ready held low;
// amplitude and all existing control/register encodings are unchanged.
// line_code is an operator-selectable PCM encoding:
//   0 = NRZ-L, 1 = NRZ-M, 2 = NRZ-S,
//   3 = Bi-phase-L, 4 = Bi-phase-M, 5 = Bi-phase-S,
//   6 = RNRZ-L (the upstream IRIG randomizer is enabled; mapping is NRZ-L).
// Bi-phase transitions occur on the half-bit boundary.  The Bessel filter is
// evaluated as a half-bit pulse convolution so those transitions are filtered
// instead of being applied as discontinuities after the filter.
// rolloff is carried for a common control-plane interface.  PCM/FM keeps its
// mandated Bessel pulse; the roll-off presets are applied by the legacy
// linear/QPSK diagnostic source in dds_inc.v.
module pcm_fm_modulator (
    input  wire               clk,
    input  wire               rst,
    input  wire               enable,
    input  wire               clear,
    input  wire               bit_data,
    input  wire               bit_valid,
    output wire               bit_ready,
    input  wire [31:0]        bit_phase,
    input  wire [31:0]        bit_rate_word,
    input  wire [31:0]        deviation_word,
    input  wire signed [31:0] frequency_offset_word,
    input  wire [15:0]        amplitude,
    input  wire [3:0]         filter_profile,
    input  wire [2:0]         line_code,
    input  wire [2:0]         rolloff,
    output reg  [11:0]        data_i,
    output reg  [11:0]        data_q,
    output wire               iq_valid,
    output reg  [31:0]        accepted_bits,
    output reg  signed [15:0] debug_nrz,
    output reg  signed [15:0] debug_filter,
    output reg  [31:0]        debug_phase
);

reg have_bit;
reg [31:0] gain_rate, gain_deviation;
reg gain_valid;
reg signed [1:0] chip_history [0:8];
reg        line_state;
reg        chip_first;
reg        chip_second;
reg        next_line_state;
reg signed [19:0] shaped_sum_early;
reg signed [19:0] shaped_sum_middle;
reg signed [19:0] shaped_sum_late;
reg signed [19:0] shaped_sum_early_pipe;
reg signed [19:0] shaped_sum_middle_pipe;
reg signed [19:0] shaped_sum_late_pipe;
reg signed [19:0] shaped_sum_pipe;
reg signed [52:0] deviation_product;
reg signed [32:0] phase_delta;
reg [31:0] phase_acc;
reg signed [31:0] i_product;
reg signed [31:0] q_product;
reg signed [15:0] sine_i_pipe, sine_q_pipe;

integer tap;

// Reset wins over acceptance in the sequential process below. Never advertise
// a transfer on that cycle: the upstream bit engine/randomizer must not advance.
assign bit_ready = enable && !rst && !clear && gain_valid &&
                   gain_rate == bit_rate_word && gain_deviation == deviation_word;
assign iq_valid  = enable && have_bit && !rst && !clear;

// Generated from the fourth-order Bessel poles in IRIG-106 Table 2-3.
// Samples are taken at the centre of each of 16 bins/Tb over 0 <= t < 3Tb.
// Sum(table)/(16*2^15) = 1.000034, corresponding to area(g)=0.500017.
function signed [16:0] bessel_pulse;
    input [5:0] index;
    begin
        case (index)
            6'd0:bessel_pulse=17'sd2;       6'd1:bessel_pulse=17'sd140;
            6'd2:bessel_pulse=17'sd822;     6'd3:bessel_pulse=17'sd2398;
            6'd4:bessel_pulse=17'sd4956;    6'd5:bessel_pulse=17'sd8337;
            6'd6:bessel_pulse=17'sd12229;   6'd7:bessel_pulse=17'sd16272;
            6'd8:bessel_pulse=17'sd20141;   6'd9:bessel_pulse=17'sd23593;
            6'd10:bessel_pulse=17'sd26485;  6'd11:bessel_pulse=17'sd28765;
            6'd12:bessel_pulse=17'sd30453;  6'd13:bessel_pulse=17'sd31621;
            6'd14:bessel_pulse=17'sd32366;  6'd15:bessel_pulse=17'sd32789;
            6'd16:bessel_pulse=17'sd32985;  6'd17:bessel_pulse=17'sd32902;
            6'd18:bessel_pulse=17'sd32193;  6'd19:bessel_pulse=17'sd30554;
            6'd20:bessel_pulse=17'sd27926;  6'd21:bessel_pulse=17'sd24484;
            6'd22:bessel_pulse=17'sd20547;  6'd23:bessel_pulse=17'sd16476;
            6'd24:bessel_pulse=17'sd12593;  6'd25:bessel_pulse=17'sd9137;
            6'd26:bessel_pulse=17'sd6249;   6'd27:bessel_pulse=17'sd3977;
            6'd28:bessel_pulse=17'sd2296;   6'd29:bessel_pulse=17'sd1136;
            6'd30:bessel_pulse=17'sd398;    6'd31:bessel_pulse=-17'sd21;
            6'd32:bessel_pulse=-17'sd217;   6'd33:bessel_pulse=-17'sd270;
            6'd34:bessel_pulse=-17'sd243;   6'd35:bessel_pulse=-17'sd181;
            6'd36:bessel_pulse=-17'sd112;   6'd37:bessel_pulse=-17'sd52;
            6'd38:bessel_pulse=-17'sd7;     6'd39:bessel_pulse=17'sd21;
            6'd40:bessel_pulse=17'sd35;     6'd41:bessel_pulse=17'sd37;
            6'd42:bessel_pulse=17'sd34;     6'd43:bessel_pulse=17'sd26;
            6'd44:bessel_pulse=17'sd18;     6'd45:bessel_pulse=17'sd11;
            6'd46:bessel_pulse=17'sd5;      default:bessel_pulse=17'sd0;
        endcase
    end
endfunction

// Response of the same fourth-order Bessel LPF to a half-bit rectangular
// chip.  It is derived as two step responses separated by Tb/2.  Repeating
// equal half-bit chips therefore reconstructs the original full-bit pulse
// while Bi-phase codes receive the correct filtered mid-bit transition.
function signed [16:0] bessel_half_pulse;
    input [5:0] index;
    begin
        case (index)
            6'd0:bessel_half_pulse=17'sd2;       6'd1:bessel_half_pulse=17'sd140;
            6'd2:bessel_half_pulse=17'sd822;     6'd3:bessel_half_pulse=17'sd2398;
            6'd4:bessel_half_pulse=17'sd4956;    6'd5:bessel_half_pulse=17'sd8337;
            6'd6:bessel_half_pulse=17'sd12229;   6'd7:bessel_half_pulse=17'sd16272;
            6'd8:bessel_half_pulse=17'sd20139;   6'd9:bessel_half_pulse=17'sd23453;
            6'd10:bessel_half_pulse=17'sd25663;  6'd11:bessel_half_pulse=17'sd26367;
            6'd12:bessel_half_pulse=17'sd25497;  6'd13:bessel_half_pulse=17'sd23284;
            6'd14:bessel_half_pulse=17'sd20137;  6'd15:bessel_half_pulse=17'sd16517;
            6'd16:bessel_half_pulse=17'sd12846;  6'd17:bessel_half_pulse=17'sd9449;
            6'd18:bessel_half_pulse=17'sd6530;   6'd19:bessel_half_pulse=17'sd4187;
            6'd20:bessel_half_pulse=17'sd2429;   6'd21:bessel_half_pulse=17'sd1200;
            6'd22:bessel_half_pulse=17'sd410;    6'd23:bessel_half_pulse=-17'sd41;
            6'd24:bessel_half_pulse=-17'sd253;   6'd25:bessel_half_pulse=-17'sd312;
            6'd26:bessel_half_pulse=-17'sd281;   6'd27:bessel_half_pulse=-17'sd210;
            6'd28:bessel_half_pulse=-17'sd133;   6'd29:bessel_half_pulse=-17'sd64;
            6'd30:bessel_half_pulse=-17'sd12;    6'd31:bessel_half_pulse=17'sd20;
            6'd32:bessel_half_pulse=17'sd36;     6'd33:bessel_half_pulse=17'sd42;
            6'd34:bessel_half_pulse=17'sd38;     6'd35:bessel_half_pulse=17'sd29;
            6'd36:bessel_half_pulse=17'sd21;     6'd37:bessel_half_pulse=17'sd12;
            6'd38:bessel_half_pulse=17'sd5;      6'd39:bessel_half_pulse=17'sd1;
            6'd40:bessel_half_pulse=-17'sd1;     6'd41:bessel_half_pulse=-17'sd5;
            6'd42:bessel_half_pulse=-17'sd4;     6'd43:bessel_half_pulse=-17'sd3;
            6'd44:bessel_half_pulse=-17'sd3;     6'd45:bessel_half_pulse=-17'sd1;
            6'd46:bessel_half_pulse=17'sd0;      6'd47:bessel_half_pulse=-17'sd1;
            default:bessel_half_pulse=17'sd0;
        endcase
    end
endfunction

// Linear interpolation between adjacent 1/16-Tb table points.  This is
// essential near the validated upper-rate region: at 15.36 Mbit/s there are
// exactly 4 samples/Tb, and a nearest-bin lookup otherwise creates a
// staircase in instantaneous frequency and excessive out-of-band power.
function signed [17:0] bessel_half_interp;
    input [5:0] index;
    input [3:0] fraction;
    reg signed [16:0] y0;
    reg signed [16:0] y1;
    reg signed [17:0] delta;
    reg signed [22:0] product;
    begin
        y0 = bessel_half_pulse(index);
        y1 = bessel_half_pulse(index + 1'b1);
        delta = $signed(y1) - $signed(y0);
        product = delta * $signed({1'b0, fraction});
        bessel_half_interp = $signed(y0) + (product >>> 4);
    end
endfunction

// Integrated analytic pulse, Q24, 32 nodes/Tb; endpoint area exactly 1/2.
function signed [25:0] integrated_pulse;
 input [8:0] index;
 begin
 case(index)
 9'd0: integrated_pulse = 26'sd0;
 9'd1: integrated_pulse = 26'sd7;
 9'd2: integrated_pulse = 26'sd211;
 9'd3: integrated_pulse = 26'sd1435;
 9'd4: integrated_pulse = 26'sd5416;
 9'd5: integrated_pulse = 26'sd14794;
 9'd6: integrated_pulse = 26'sd32938;
 9'd7: integrated_pulse = 26'sd63674;
 9'd8: integrated_pulse = 26'sd111006;
 9'd9: integrated_pulse = 26'sd178832;
 9'd10: integrated_pulse = 26'sd270716;
 9'd11: integrated_pulse = 26'sd389701;
 9'd12: integrated_pulse = 26'sd538190;
 9'd13: integrated_pulse = 26'sd717873;
 9'd14: integrated_pulse = 26'sd929708;
 9'd15: integrated_pulse = 26'sd1173940;
 9'd16: integrated_pulse = 26'sd1450157;
 9'd17: integrated_pulse = 26'sd1757362;
 9'd18: integrated_pulse = 26'sd2093883;
 9'd19: integrated_pulse = 26'sd2457029;
 9'd20: integrated_pulse = 26'sd2842908;
 9'd21: integrated_pulse = 26'sd3246532;
 9'd22: integrated_pulse = 26'sd3662090;
 9'd23: integrated_pulse = 26'sd4083285;
 9'd24: integrated_pulse = 26'sd4503697;
 9'd25: integrated_pulse = 26'sd4917111;
 9'd26: integrated_pulse = 26'sd5317792;
 9'd27: integrated_pulse = 26'sd5700700;
 9'd28: integrated_pulse = 26'sd6061634;
 9'd29: integrated_pulse = 26'sd6397311;
 9'd30: integrated_pulse = 26'sd6705387;
 9'd31: integrated_pulse = 26'sd6984435;
 9'd32: integrated_pulse = 26'sd7233879;
 9'd33: integrated_pulse = 26'sd7453903;
 9'd34: integrated_pulse = 26'sd7645339;
 9'd35: integrated_pulse = 26'sd7809556;
 9'd36: integrated_pulse = 26'sd7948334;
 9'd37: integrated_pulse = 26'sd8063750;
 9'd38: integrated_pulse = 26'sd8158069;
 9'd39: integrated_pulse = 26'sd8233652;
 9'd40: integrated_pulse = 26'sd8292867;
 9'd41: integrated_pulse = 26'sd8338024;
 9'd42: integrated_pulse = 26'sd8371320;
 9'd43: integrated_pulse = 26'sd8394796;
 9'd44: integrated_pulse = 26'sd8410312;
 9'd45: integrated_pulse = 26'sd8419529;
 9'd46: integrated_pulse = 26'sd8423900;
 9'd47: integrated_pulse = 26'sd8424673;
 9'd48: integrated_pulse = 26'sd8422899;
 9'd49: integrated_pulse = 26'sd8419441;
 9'd50: integrated_pulse = 26'sd8414991;
 9'd51: integrated_pulse = 26'sd8410092;
 9'd52: integrated_pulse = 26'sd8405149;
 9'd53: integrated_pulse = 26'sd8400456;
 9'd54: integrated_pulse = 26'sd8396208;
 9'd55: integrated_pulse = 26'sd8392524;
 9'd56: integrated_pulse = 26'sd8389459;
 9'd57: integrated_pulse = 26'sd8387024;
 9'd58: integrated_pulse = 26'sd8385190;
 9'd59: integrated_pulse = 26'sd8383907;
 9'd60: integrated_pulse = 26'sd8383110;
 9'd61: integrated_pulse = 26'sd8382726;
 9'd62: integrated_pulse = 26'sd8382677;
 9'd63: integrated_pulse = 26'sd8382890;
 9'd64: integrated_pulse = 26'sd8383297;
 9'd65: integrated_pulse = 26'sd8383836;
 9'd66: integrated_pulse = 26'sd8384454;
 9'd67: integrated_pulse = 26'sd8385107;
 9'd68: integrated_pulse = 26'sd8385759;
 9'd69: integrated_pulse = 26'sd8386382;
 9'd70: integrated_pulse = 26'sd8386957;
 9'd71: integrated_pulse = 26'sd8387470;
 9'd72: integrated_pulse = 26'sd8387915;
 9'd73: integrated_pulse = 26'sd8388287;
 9'd74: integrated_pulse = 26'sd8388589;
 9'd75: integrated_pulse = 26'sd8388824;
 9'd76: integrated_pulse = 26'sd8388997;
 9'd77: integrated_pulse = 26'sd8389116;
 9'd78: integrated_pulse = 26'sd8389187;
 9'd79: integrated_pulse = 26'sd8389220;
 9'd80: integrated_pulse = 26'sd8389221;
 9'd81: integrated_pulse = 26'sd8389198;
 9'd82: integrated_pulse = 26'sd8389156;
 9'd83: integrated_pulse = 26'sd8389103;
 9'd84: integrated_pulse = 26'sd8389043;
 9'd85: integrated_pulse = 26'sd8388980;
 9'd86: integrated_pulse = 26'sd8388917;
 9'd87: integrated_pulse = 26'sd8388857;
 9'd88: integrated_pulse = 26'sd8388801;
 9'd89: integrated_pulse = 26'sd8388751;
 9'd90: integrated_pulse = 26'sd8388707;
 9'd91: integrated_pulse = 26'sd8388670;
 9'd92: integrated_pulse = 26'sd8388639;
 9'd93: integrated_pulse = 26'sd8388615;
 9'd94: integrated_pulse = 26'sd8388596;
 9'd95: integrated_pulse = 26'sd8388583;
 9'd96: integrated_pulse = 26'sd8388573;
 9'd97: integrated_pulse = 26'sd8388568;
 9'd98: integrated_pulse = 26'sd8388566;
 9'd99: integrated_pulse = 26'sd8388566;
 9'd100: integrated_pulse = 26'sd8388568;
 9'd101: integrated_pulse = 26'sd8388571;
 9'd102: integrated_pulse = 26'sd8388575;
 9'd103: integrated_pulse = 26'sd8388579;
 9'd104: integrated_pulse = 26'sd8388584;
 9'd105: integrated_pulse = 26'sd8388589;
 9'd106: integrated_pulse = 26'sd8388593;
 9'd107: integrated_pulse = 26'sd8388597;
 9'd108: integrated_pulse = 26'sd8388600;
 9'd109: integrated_pulse = 26'sd8388603;
 9'd110: integrated_pulse = 26'sd8388606;
 9'd111: integrated_pulse = 26'sd8388608;
 9'd112: integrated_pulse = 26'sd8388609;
 9'd113: integrated_pulse = 26'sd8388610;
 9'd114: integrated_pulse = 26'sd8388611;
 9'd115: integrated_pulse = 26'sd8388611;
 9'd116: integrated_pulse = 26'sd8388611;
 9'd117: integrated_pulse = 26'sd8388611;
 9'd118: integrated_pulse = 26'sd8388611;
 9'd119: integrated_pulse = 26'sd8388611;
 9'd120: integrated_pulse = 26'sd8388611;
 9'd121: integrated_pulse = 26'sd8388610;
 9'd122: integrated_pulse = 26'sd8388610;
 9'd123: integrated_pulse = 26'sd8388609;
 9'd124: integrated_pulse = 26'sd8388609;
 9'd125: integrated_pulse = 26'sd8388609;
 9'd126: integrated_pulse = 26'sd8388608;
 9'd127: integrated_pulse = 26'sd8388608;
 9'd128: integrated_pulse = 26'sd8388608;
 default: integrated_pulse = 26'sd8388608;
 endcase
 end
endfunction

// Configure deviation/Rb in Q32 using a serial divider, not a combinational
// divide on the sample path. Configuration takes 64 clocks; backpressure
// prevents accepting a BIT until the ratio is ready.
reg [63:0] gain_word, divide_work;
reg [32:0] divide_remainder;
reg [6:0] divide_count;
wire [32:0] divide_trial = {divide_remainder[31:0],divide_work[63]};
wire divide_ge = divide_trial >= {1'b0,gain_rate};
always @(posedge clk) begin
 if (rst) begin
  gain_rate<=0; gain_deviation<=0; gain_word<=0; divide_work<=0;
  divide_remainder<=0; divide_count<=0; gain_valid<=0;
 end else if (gain_rate != bit_rate_word || gain_deviation != deviation_word) begin
  gain_rate<=bit_rate_word; gain_deviation<=deviation_word;
  divide_work<={deviation_word,32'd0}; divide_remainder<=0;
  divide_count<=64; gain_valid<=0;
 end else if (divide_count != 0) begin
  divide_work<={divide_work[62:0],divide_ge};
  divide_remainder<=divide_ge ? divide_trial-{1'b0,gain_rate} : divide_trial;
  divide_count<=divide_count-1'b1;
  if (divide_count==1) begin
   gain_word<={divide_work[62:0],divide_ge};
   gain_valid<=gain_rate!=0;
  end
 end
end
reg signed [31:0] completed_area, area_base, area_next;
reg signed [31:0] area_base_pipe, area_next_pipe, area_total, area_previous;
reg signed [31:0] completed_area_pipe;
reg [11:0] area_fraction_pipe;
reg [11:0] area_fraction_input;
reg signed [31:0] completed_area_input;
reg signed [25:0] tap_base [0:8], tap_next [0:8];
reg signed [25:0] tap_base_pipe [0:8], tap_next_pipe [0:8];
integer area_reg;
wire signed [32:0] area_difference =
 $signed({area_next_pipe[31],area_next_pipe})-$signed({area_base_pipe[31],area_base_pipe});
wire signed [45:0] area_interpolation = area_difference*$signed({1'b0,area_fraction_pipe});
wire signed [31:0] area_increment = area_total-area_previous;
reg signed [96:0] area_product;
reg [23:0] phase_residue;
wire signed [96:0] exact_increment = area_product+$signed({1'b0,phase_residue});
integer area_tap;
reg [8:0] area_index;
always @* begin
 area_index=0;
 for (area_tap=0;area_tap<9;area_tap=area_tap+1) begin
  tap_base[area_tap]=0;tap_next[area_tap]=0;
  if (area_tap!=0 || bit_phase[31]) begin
   if (area_tap==0) area_index={4'd0,bit_phase[31:27]}-9'd16;
   else area_index={4'd0,bit_phase[31:27]}+(area_tap-1)*16;
   if (chip_history[area_tap]==2'sd1) begin
    tap_base[area_tap]=integrated_pulse(area_index);
    tap_next[area_tap]=integrated_pulse(area_index+1'b1);
   end else if (chip_history[area_tap]==-2'sd1) begin
    tap_base[area_tap]=-integrated_pulse(area_index);
    tap_next[area_tap]=-integrated_pulse(area_index+1'b1);
   end
  end
 end
 area_base = ((tap_base_pipe[0]+tap_base_pipe[1])+(tap_base_pipe[2]+tap_base_pipe[3])) +
             ((tap_base_pipe[4]+tap_base_pipe[5])+(tap_base_pipe[6]+tap_base_pipe[7])) + tap_base_pipe[8];
 area_next = ((tap_next_pipe[0]+tap_next_pipe[1])+(tap_next_pipe[2]+tap_next_pipe[3])) +
             ((tap_next_pipe[4]+tap_next_pipe[5])+(tap_next_pipe[6]+tap_next_pipe[7])) + tap_next_pipe[8];
end

function signed [15:0] sine_lut;
    input [7:0] phase;
    reg [6:0] x;
    reg signed [15:0] y;
    begin
        case (phase[7:6])
            2'b00: x = {1'b0, phase[5:0]};
            2'b01: x = 7'd64 - {1'b0, phase[5:0]};
            2'b10: x = {1'b0, phase[5:0]};
            default: x = 7'd64 - {1'b0, phase[5:0]};
        endcase
        case (x)
            7'd0: y=16'sd0; 7'd1: y=16'sd804; 7'd2: y=16'sd1608; 7'd3: y=16'sd2410;
            7'd4: y=16'sd3212; 7'd5: y=16'sd4011; 7'd6: y=16'sd4808; 7'd7: y=16'sd5602;
            7'd8: y=16'sd6393; 7'd9: y=16'sd7179; 7'd10:y=16'sd7962; 7'd11:y=16'sd8739;
            7'd12:y=16'sd9512; 7'd13:y=16'sd10278;7'd14:y=16'sd11039;7'd15:y=16'sd11793;
            7'd16:y=16'sd12539;7'd17:y=16'sd13279;7'd18:y=16'sd14010;7'd19:y=16'sd14732;
            7'd20:y=16'sd15446;7'd21:y=16'sd16151;7'd22:y=16'sd16846;7'd23:y=16'sd17530;
            7'd24:y=16'sd18204;7'd25:y=16'sd18868;7'd26:y=16'sd19519;7'd27:y=16'sd20159;
            7'd28:y=16'sd20787;7'd29:y=16'sd21403;7'd30:y=16'sd22005;7'd31:y=16'sd22594;
            7'd32:y=16'sd23170;7'd33:y=16'sd23731;7'd34:y=16'sd24279;7'd35:y=16'sd24811;
            7'd36:y=16'sd25329;7'd37:y=16'sd25832;7'd38:y=16'sd26319;7'd39:y=16'sd26790;
            7'd40:y=16'sd27245;7'd41:y=16'sd27683;7'd42:y=16'sd28105;7'd43:y=16'sd28510;
            7'd44:y=16'sd28898;7'd45:y=16'sd29268;7'd46:y=16'sd29621;7'd47:y=16'sd29956;
            7'd48:y=16'sd30273;7'd49:y=16'sd30571;7'd50:y=16'sd30852;7'd51:y=16'sd31113;
            7'd52:y=16'sd31356;7'd53:y=16'sd31580;7'd54:y=16'sd31785;7'd55:y=16'sd31971;
            7'd56:y=16'sd32137;7'd57:y=16'sd32285;7'd58:y=16'sd32412;7'd59:y=16'sd32521;
            7'd60:y=16'sd32609;7'd61:y=16'sd32678;7'd62:y=16'sd32728;7'd63:y=16'sd32757;
            default:y=16'sd32767;
        endcase
        if (phase[7]) sine_lut = -y; else sine_lut = y;
    end
endfunction

// 16-bit phase lookup, with a register before amplitude multiplication.
// Adds one sample of I/Q latency; it does not alter BIT timing or phase rate.
function signed [15:0] sine_interpolated;
    input [15:0] phase;
    reg signed [15:0] y0, y1;
    reg signed [16:0] delta;
    reg signed [25:0] product;
    begin
        y0 = sine_lut(phase[15:8]);
        y1 = sine_lut(phase[15:8] + 8'd1);
        delta = $signed({y1[15],y1}) - $signed({y0[15],y0});
        product = delta * $signed({1'b0,phase[7:0]});
        sine_interpolated = $signed(y0) + ((product + 26'sd128) >>> 8);
    end
endfunction

function [11:0] saturate_12;
    input signed [31:0] value;
    begin
        if (value > 32'sd2047) saturate_12 = 12'h7ff;
        else if (value < -32'sd2048) saturate_12 = 12'h800;
        else saturate_12 = value[11:0];
    end
endfunction

always @* begin
    // chip_history[1:0] are the first/second halves of the current bit;
    // [3:2], [5:4] are the preceding bits and [6] is the older second half.
    // Register independent 2+2+3-tap groups.  In particular, the first four
    // taps must not form a serial carry chain from the fractional bit phase
    // to one register at 61.44 MSPS.  All groups still sum one cycle later,
    // preserving the debug-filter latency and numerical order.
    shaped_sum_early = 20'sd0;
    shaped_sum_middle = 20'sd0;
    shaped_sum_late = 20'sd0;
    if (chip_history[1] == 2'sd1)
        shaped_sum_early = shaped_sum_early + bessel_half_interp(
            {2'd0,bit_phase[31:28]}, bit_phase[27:24]);
    else if (chip_history[1] == -2'sd1)
        shaped_sum_early = shaped_sum_early - bessel_half_interp(
            {2'd0,bit_phase[31:28]}, bit_phase[27:24]);
    if (bit_phase[31]) begin
        if (chip_history[0] == 2'sd1)
            shaped_sum_early = shaped_sum_early + bessel_half_interp(
                {3'd0,bit_phase[30:28]}, bit_phase[27:24]);
        else if (chip_history[0] == -2'sd1)
            shaped_sum_early = shaped_sum_early - bessel_half_interp(
                {3'd0,bit_phase[30:28]}, bit_phase[27:24]);
    end
    for (tap=2; tap<4; tap=tap+1) begin
        if (chip_history[tap] == 2'sd1)
            shaped_sum_middle = shaped_sum_middle + bessel_half_interp(
                bit_phase[31:28] + ((tap-1) << 3), bit_phase[27:24]);
        else if (chip_history[tap] == -2'sd1)
            shaped_sum_middle = shaped_sum_middle - bessel_half_interp(
                bit_phase[31:28] + ((tap-1) << 3), bit_phase[27:24]);
    end
    for (tap=4; tap<7; tap=tap+1) begin
        if (chip_history[tap] == 2'sd1)
            shaped_sum_late = shaped_sum_late + bessel_half_interp(
                bit_phase[31:28] + ((tap-1) << 3), bit_phase[27:24]);
        else if (chip_history[tap] == -2'sd1)
            shaped_sum_late = shaped_sum_late - bessel_half_interp(
                bit_phase[31:28] + ((tap-1) << 3), bit_phase[27:24]);
    end
end

// Convert one source bit into the two half-bit levels used by the selected
// code.  line_state is the level at the end of the preceding bit.
always @* begin
    chip_first = bit_data;
    chip_second = bit_data;
    next_line_state = bit_data;
    case (line_code)
        3'd1: begin                         // NRZ-M: transition on mark/one
            chip_first = line_state ^ bit_data;
            chip_second = chip_first;
            next_line_state = chip_second;
        end
        3'd2: begin                         // NRZ-S: transition on space/zero
            chip_first = line_state ^ ~bit_data;
            chip_second = chip_first;
            next_line_state = chip_second;
        end
        3'd3: begin                         // Bi-phase-L (Manchester)
            chip_first = bit_data;
            chip_second = ~bit_data;
            next_line_state = chip_second;
        end
        3'd4: begin                         // Bi-phase-M
            chip_first = line_state ^ ~bit_data; // IRIG: start transition on zero
            chip_second = ~chip_first;      // midpoint transition on every bit
            next_line_state = chip_second;
        end
        3'd5: begin                         // Bi-phase-S
            chip_first = line_state ^ bit_data; // IRIG: start transition on one
            chip_second = ~chip_first;
            next_line_state = chip_second;
        end
        3'd6: begin                         // RNRZ-L: randomization is upstream
            chip_first = bit_data;
            chip_second = bit_data;
            next_line_state = chip_second;
        end
        default: begin                      // NRZ-L
            chip_first = bit_data;
            chip_second = bit_data;
            next_line_state = chip_second;
        end
    endcase
end

wire current_chip = bit_phase[31] ? (chip_history[0] == 2'sd1) :
                                    (chip_history[1] == 2'sd1);

always @(posedge clk) begin
    if (rst || clear || !enable || !gain_valid) begin
        completed_area<=0; area_base_pipe<=0; area_next_pipe<=0;
        completed_area_pipe<=0; area_fraction_pipe<=0;
        area_total<=0; area_previous<=0; area_product<=0; phase_residue<=0;
        completed_area_input<=0;area_fraction_input<=0;
        for(area_reg=0;area_reg<9;area_reg=area_reg+1) begin
            tap_base_pipe[area_reg]<=0;tap_next_pipe[area_reg]<=0;
        end
        have_bit       <= 1'b0;
        accepted_bits  <= 32'd0;
        debug_nrz      <= 16'sd0;
        debug_filter   <= 16'sd0;
        line_state     <= 1'b0;
        shaped_sum_early_pipe <= 20'sd0;
        shaped_sum_middle_pipe <= 20'sd0;
        shaped_sum_late_pipe <= 20'sd0;
        shaped_sum_pipe <= 20'sd0;
        deviation_product <= 53'sd0;
        phase_delta    <= 33'sd0;
        phase_acc      <= 32'd0;
        debug_phase    <= 32'd0;
        i_product      <= 32'sd0;
        q_product      <= 32'sd0;
        sine_i_pipe    <= 16'sd0;
        sine_q_pipe    <= 16'sd0;
        data_i         <= 12'd0;
        data_q         <= 12'd0;
        for (tap=0; tap<9; tap=tap+1)
            chip_history[tap] <= 2'sd0;
    end else begin
        if (bit_valid && bit_ready) begin
            have_bit      <= 1'b1;
            accepted_bits <= accepted_bits + 1'b1;
            completed_area <= completed_area +
                (chip_history[7]==2'sd1 ? 32'sd8388608 : chip_history[7]==-2'sd1 ? -32'sd8388608 : 32'sd0) +
                (chip_history[8]==2'sd1 ? 32'sd8388608 : chip_history[8]==-2'sd1 ? -32'sd8388608 : 32'sd0);
            chip_history[8] <= chip_history[6];
            chip_history[7] <= chip_history[5];
            chip_history[6] <= chip_history[4];
            chip_history[5] <= chip_history[3];
            chip_history[4] <= chip_history[2];
            chip_history[3] <= chip_history[1];
            chip_history[2] <= chip_history[0];
            chip_history[1] <= chip_first ? 2'sd1 : -2'sd1;
            chip_history[0] <= chip_second ? 2'sd1 : -2'sd1;
            line_state <= next_line_state;
        end

        if (have_bit)
            debug_nrz <= current_chip ? 16'sd32767 : -16'sd32767;

        shaped_sum_early_pipe <= shaped_sum_early;
        shaped_sum_middle_pipe <= shaped_sum_middle;
        shaped_sum_late_pipe <= shaped_sum_late;
        shaped_sum_pipe <= (shaped_sum_early_pipe + shaped_sum_middle_pipe)
                           + shaped_sum_late_pipe;
        if (shaped_sum_pipe > 20'sd32767) debug_filter <= 16'sd32767;
        else if (shaped_sum_pipe < -20'sd32768) debug_filter <= -16'sd32768;
        else debug_filter <= shaped_sum_pipe[15:0];

        // Q1.15 normalized Bessel output times the programmed peak
        // deviation word.  A steady run settles to +/- deviation_word.
        deviation_product <= $signed(shaped_sum_pipe) *
                             $signed({1'b0, deviation_word});
        phase_delta <= deviation_product >>> 15;
        // Evaluate integrated pulse areas, then difference successive samples.
        // Completed tails preserve continuity across BIT/replay boundaries.
        for(area_reg=0;area_reg<9;area_reg=area_reg+1) begin
            tap_base_pipe[area_reg]<=tap_base[area_reg];
            tap_next_pipe[area_reg]<=tap_next[area_reg];
        end
        completed_area_input<=completed_area;
        area_fraction_input<=bit_phase[26:15];
        area_base_pipe <= area_base;
        area_next_pipe <= area_next;
        completed_area_pipe <= completed_area_input;
        area_fraction_pipe <= area_fraction_input;
        area_total <= completed_area_pipe + area_base_pipe + ((area_interpolation + 46'sd2048) >>> 12);
        area_previous <= area_total;
        area_product <= area_increment * $signed({1'b0,gain_word});
        phase_residue <= exact_increment[23:0];
        phase_acc <= phase_acc + frequency_offset_word + (exact_increment >>> 24);
        debug_phase <= phase_acc;

        sine_i_pipe <= sine_interpolated(phase_acc[31:16] + 16'h4000);
        sine_q_pipe <= sine_interpolated(phase_acc[31:16]);
        i_product <= sine_i_pipe * $signed({1'b0, amplitude[14:0]});
        q_product <= sine_q_pipe * $signed({1'b0, amplitude[14:0]});
        if (have_bit) begin
            // Q1.15 sine * Q1.15 amplitude -> signed 12-bit full scale.
            data_i <= saturate_12(i_product >>> 19);
            data_q <= saturate_12(q_product >>> 19);
        end else begin
            data_i <= 12'd0;
            data_q <= 12'd0;
        end
    end
end

// Keep these controls referenced in simulation/synthesis even though the
// Bessel pulse itself is deliberately independent of roll-off.
wire unused_filter_profile = &{1'b0,filter_profile};
wire unused_rolloff = &{1'b0,rolloff};
wire unused_bit_rate_word = &{1'b0,bit_rate_word};

endmodule
