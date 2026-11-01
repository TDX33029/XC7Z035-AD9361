`timescale 1 ns / 1 ps

// IRIG-106 SOQPSK-TG complex-baseband transmitter.
//
// Processing chain:
//   optional differential encoder -> constrained ternary precoder ->
//   integrated 8-Tb TG phase response -> h=1/2 -> constant-envelope I/Q.
//
// bit_rate_word = round(bit_rate / 61.44e6 * 2^32).  The frequency-pulse
// diagnostic table contains 129 Q1.15 frequency nodes at Tb/16. Transmitted
// phase uses 257 Q24 integrated nodes at Tb/32 and 12-bit interpolation.
// Completed pulse tails are accumulated exactly, instead of integrating
// quantized instantaneous frequency. Area is 1/2; rho=.70, B=1.25, T1=1.5,
// T2=.5 as specified by IRIG-106.  A four-bit causal delay implements the
// standard pulse whose mathematical definition is centred on t=0.  The
// shared project ceiling is 20 Mbit/s (3.072 complex samples per BIT).
module soqpsk_tg_modulator (
    input  wire               clk,
    input  wire               rst,
    input  wire               enable,
    input  wire               clear,
    input  wire               differential_enable,
    input  wire               bit_data,
    input  wire               bit_valid,
    output wire               bit_ready,
    input  wire [31:0]        bit_phase,
    input  wire [31:0]        bit_rate_word,
    input  wire signed [31:0] frequency_offset_word,
    input  wire [15:0]        amplitude,
    output reg  [11:0]        data_i,
    output reg  [11:0]        data_q,
    output wire               iq_valid,
    output reg  [31:0]        accepted_bits,
    output reg  signed [15:0] debug_alpha,
    output reg  signed [15:0] debug_frequency,
    output reg  [31:0]        debug_phase
);

reg have_bit;
reg bit_pair_phase;
reg delta_even;
reg delta_prev_odd;
reg symbol_parity;
reg signed [1:0] a_prev1;
reg signed [1:0] a_prev2;
reg signed [1:0] alpha_history [0:7];
reg signed [18:0] shaped_sum;
reg signed [18:0] shaped_next;
reg signed [18:0] shaped_base_pipe, shaped_next_pipe;
reg [7:0] pulse_fraction_pipe;
reg signed [19:0] pulse_delta;
reg signed [28:0] pulse_interpolation;
reg signed [15:0] sine_i_pipe, sine_q_pipe;
reg signed [18:0] shaped_sum_pipe;
reg signed [50:0] phase_product;
reg [31:0] phase_acc;
reg signed [31:0] i_product;
reg signed [31:0] q_product;

integer k;

assign bit_ready = enable && !rst && !clear;
assign iq_valid = enable && have_bit && !rst && !clear;

wire encoded_bit = differential_enable ?
    (bit_pair_phase ? (bit_data ^ delta_even) : (bit_data ^ ~delta_prev_odd)) :
    bit_data;
wire signed [1:0] a_current = encoded_bit ? 2'sd1 : -2'sd1;
wire signed [2:0] a_difference = $signed(a_current) - $signed(a_prev2);
wire signed [4:0] alpha_product = $signed(a_prev1) * $signed(a_difference);
wire signed [2:0] alpha_unphased = alpha_product >>> 1;
wire signed [1:0] alpha_next = symbol_parity ?
    alpha_unphased[1:0] : -$signed(alpha_unphased[1:0]);

// IRIG-106-24 Eq. 2-19..21: 129 nodes from -4Tb to +4Tb at Tb/16.
// Endpoints are zero; interpolation spans adjacent nodes without a half-bin shift.
function signed [15:0] frequency_pulse;
    input [7:0] index;
    begin
        case (index)
            8'd0: frequency_pulse=16'sd0;
            8'd1: frequency_pulse=-16'sd1;
            8'd2: frequency_pulse=-16'sd3;
            8'd3: frequency_pulse=-16'sd5;
            8'd4: frequency_pulse=-16'sd8;
            8'd5: frequency_pulse=-16'sd9;
            8'd6: frequency_pulse=-16'sd10;
            8'd7: frequency_pulse=-16'sd8;
            8'd8: frequency_pulse=-16'sd5;
            8'd9: frequency_pulse=-16'sd1;
            8'd10: frequency_pulse=16'sd3;
            8'd11: frequency_pulse=16'sd6;
            8'd12: frequency_pulse=16'sd5;
            8'd13: frequency_pulse=-16'sd2;
            8'd14: frequency_pulse=-16'sd15;
            8'd15: frequency_pulse=-16'sd35;
            8'd16: frequency_pulse=-16'sd62;
            8'd17: frequency_pulse=-16'sd97;
            8'd18: frequency_pulse=-16'sd139;
            8'd19: frequency_pulse=-16'sd189;
            8'd20: frequency_pulse=-16'sd245;
            8'd21: frequency_pulse=-16'sd307;
            8'd22: frequency_pulse=-16'sd374;
            8'd23: frequency_pulse=-16'sd444;
            8'd24: frequency_pulse=-16'sd515;
            8'd25: frequency_pulse=-16'sd586;
            8'd26: frequency_pulse=-16'sd653;
            8'd27: frequency_pulse=-16'sd715;
            8'd28: frequency_pulse=-16'sd767;
            8'd29: frequency_pulse=-16'sd807;
            8'd30: frequency_pulse=-16'sd832;
            8'd31: frequency_pulse=-16'sd838;
            8'd32: frequency_pulse=-16'sd822;
            8'd33: frequency_pulse=-16'sd781;
            8'd34: frequency_pulse=-16'sd711;
            8'd35: frequency_pulse=-16'sd610;
            8'd36: frequency_pulse=-16'sd475;
            8'd37: frequency_pulse=-16'sd303;
            8'd38: frequency_pulse=-16'sd94;
            8'd39: frequency_pulse=16'sd153;
            8'd40: frequency_pulse=16'sd441;
            8'd41: frequency_pulse=16'sd767;
            8'd42: frequency_pulse=16'sd1132;
            8'd43: frequency_pulse=16'sd1534;
            8'd44: frequency_pulse=16'sd1970;
            8'd45: frequency_pulse=16'sd2438;
            8'd46: frequency_pulse=16'sd2935;
            8'd47: frequency_pulse=16'sd3455;
            8'd48: frequency_pulse=16'sd3993;
            8'd49: frequency_pulse=16'sd4546;
            8'd50: frequency_pulse=16'sd5106;
            8'd51: frequency_pulse=16'sd5666;
            8'd52: frequency_pulse=16'sd6222;
            8'd53: frequency_pulse=16'sd6765;
            8'd54: frequency_pulse=16'sd7290;
            8'd55: frequency_pulse=16'sd7789;
            8'd56: frequency_pulse=16'sd8255;
            8'd57: frequency_pulse=16'sd8684;
            8'd58: frequency_pulse=16'sd9068;
            8'd59: frequency_pulse=16'sd9402;
            8'd60: frequency_pulse=16'sd9683;
            8'd61: frequency_pulse=16'sd9905;
            8'd62: frequency_pulse=16'sd10066;
            8'd63: frequency_pulse=16'sd10164;
            8'd64: frequency_pulse=16'sd10196;
            8'd65: frequency_pulse=16'sd10164;
            8'd66: frequency_pulse=16'sd10066;
            8'd67: frequency_pulse=16'sd9905;
            8'd68: frequency_pulse=16'sd9683;
            8'd69: frequency_pulse=16'sd9402;
            8'd70: frequency_pulse=16'sd9068;
            8'd71: frequency_pulse=16'sd8684;
            8'd72: frequency_pulse=16'sd8255;
            8'd73: frequency_pulse=16'sd7789;
            8'd74: frequency_pulse=16'sd7290;
            8'd75: frequency_pulse=16'sd6765;
            8'd76: frequency_pulse=16'sd6222;
            8'd77: frequency_pulse=16'sd5666;
            8'd78: frequency_pulse=16'sd5106;
            8'd79: frequency_pulse=16'sd4546;
            8'd80: frequency_pulse=16'sd3993;
            8'd81: frequency_pulse=16'sd3455;
            8'd82: frequency_pulse=16'sd2935;
            8'd83: frequency_pulse=16'sd2438;
            8'd84: frequency_pulse=16'sd1970;
            8'd85: frequency_pulse=16'sd1534;
            8'd86: frequency_pulse=16'sd1132;
            8'd87: frequency_pulse=16'sd767;
            8'd88: frequency_pulse=16'sd441;
            8'd89: frequency_pulse=16'sd153;
            8'd90: frequency_pulse=-16'sd94;
            8'd91: frequency_pulse=-16'sd303;
            8'd92: frequency_pulse=-16'sd475;
            8'd93: frequency_pulse=-16'sd610;
            8'd94: frequency_pulse=-16'sd711;
            8'd95: frequency_pulse=-16'sd781;
            8'd96: frequency_pulse=-16'sd822;
            8'd97: frequency_pulse=-16'sd838;
            8'd98: frequency_pulse=-16'sd832;
            8'd99: frequency_pulse=-16'sd807;
            8'd100: frequency_pulse=-16'sd767;
            8'd101: frequency_pulse=-16'sd715;
            8'd102: frequency_pulse=-16'sd653;
            8'd103: frequency_pulse=-16'sd586;
            8'd104: frequency_pulse=-16'sd515;
            8'd105: frequency_pulse=-16'sd444;
            8'd106: frequency_pulse=-16'sd374;
            8'd107: frequency_pulse=-16'sd307;
            8'd108: frequency_pulse=-16'sd245;
            8'd109: frequency_pulse=-16'sd189;
            8'd110: frequency_pulse=-16'sd139;
            8'd111: frequency_pulse=-16'sd97;
            8'd112: frequency_pulse=-16'sd62;
            8'd113: frequency_pulse=-16'sd35;
            8'd114: frequency_pulse=-16'sd15;
            8'd115: frequency_pulse=-16'sd2;
            8'd116: frequency_pulse=16'sd5;
            8'd117: frequency_pulse=16'sd6;
            8'd118: frequency_pulse=16'sd3;
            8'd119: frequency_pulse=-16'sd1;
            8'd120: frequency_pulse=-16'sd5;
            8'd121: frequency_pulse=-16'sd8;
            8'd122: frequency_pulse=-16'sd10;
            8'd123: frequency_pulse=-16'sd9;
            8'd124: frequency_pulse=-16'sd8;
            8'd125: frequency_pulse=-16'sd5;
            8'd126: frequency_pulse=-16'sd3;
            8'd127: frequency_pulse=-16'sd1;
            8'd128: frequency_pulse=16'sd0;
            default: frequency_pulse=16'sd0;
        endcase
    end
endfunction

// Integrated analytic pulse, Q24, 32 nodes/Tb; endpoint area exactly 1/2.
function signed [25:0] integrated_pulse;
 input [8:0] index;
 begin
 case(index)
 9'd0: integrated_pulse = 26'sd0;
 9'd1: integrated_pulse = -26'sd1;
 9'd2: integrated_pulse = -26'sd8;
 9'd3: integrated_pulse = -26'sd27;
 9'd4: integrated_pulse = -26'sd61;
 9'd5: integrated_pulse = -26'sd114;
 9'd6: integrated_pulse = -26'sd189;
 9'd7: integrated_pulse = -26'sd284;
 9'd8: integrated_pulse = -26'sd400;
 9'd9: integrated_pulse = -26'sd534;
 9'd10: integrated_pulse = -26'sd681;
 9'd11: integrated_pulse = -26'sd835;
 9'd12: integrated_pulse = -26'sd991;
 9'd13: integrated_pulse = -26'sd1141;
 9'd14: integrated_pulse = -26'sd1279;
 9'd15: integrated_pulse = -26'sd1396;
 9'd16: integrated_pulse = -26'sd1488;
 9'd17: integrated_pulse = -26'sd1549;
 9'd18: integrated_pulse = -26'sd1577;
 9'd19: integrated_pulse = -26'sd1569;
 9'd20: integrated_pulse = -26'sd1530;
 9'd21: integrated_pulse = -26'sd1462;
 9'd22: integrated_pulse = -26'sd1376;
 9'd23: integrated_pulse = -26'sd1283;
 9'd24: integrated_pulse = -26'sd1200;
 9'd25: integrated_pulse = -26'sd1144;
 9'd26: integrated_pulse = -26'sd1138;
 9'd27: integrated_pulse = -26'sd1209;
 9'd28: integrated_pulse = -26'sd1382;
 9'd29: integrated_pulse = -26'sd1688;
 9'd30: integrated_pulse = -26'sd2156;
 9'd31: integrated_pulse = -26'sd2816;
 9'd32: integrated_pulse = -26'sd3697;
 9'd33: integrated_pulse = -26'sd4825;
 9'd34: integrated_pulse = -26'sd6230;
 9'd35: integrated_pulse = -26'sd7942;
 9'd36: integrated_pulse = -26'sd9992;
 9'd37: integrated_pulse = -26'sd12408;
 9'd38: integrated_pulse = -26'sd15219;
 9'd39: integrated_pulse = -26'sd18453;
 9'd40: integrated_pulse = -26'sd22136;
 9'd41: integrated_pulse = -26'sd26293;
 9'd42: integrated_pulse = -26'sd30947;
 9'd43: integrated_pulse = -26'sd36118;
 9'd44: integrated_pulse = -26'sd41823;
 9'd45: integrated_pulse = -26'sd48077;
 9'd46: integrated_pulse = -26'sd54892;
 9'd47: integrated_pulse = -26'sd62275;
 9'd48: integrated_pulse = -26'sd70230;
 9'd49: integrated_pulse = -26'sd78755;
 9'd50: integrated_pulse = -26'sd87847;
 9'd51: integrated_pulse = -26'sd97493;
 9'd52: integrated_pulse = -26'sd107680;
 9'd53: integrated_pulse = -26'sd118385;
 9'd54: integrated_pulse = -26'sd129582;
 9'd55: integrated_pulse = -26'sd141238;
 9'd56: integrated_pulse = -26'sd153315;
 9'd57: integrated_pulse = -26'sd165767;
 9'd58: integrated_pulse = -26'sd178541;
 9'd59: integrated_pulse = -26'sd191581;
 9'd60: integrated_pulse = -26'sd204821;
 9'd61: integrated_pulse = -26'sd218189;
 9'd62: integrated_pulse = -26'sd231606;
 9'd63: integrated_pulse = -26'sd244988;
 9'd64: integrated_pulse = -26'sd258243;
 9'd65: integrated_pulse = -26'sd271271;
 9'd66: integrated_pulse = -26'sd283969;
 9'd67: integrated_pulse = -26'sd296225;
 9'd68: integrated_pulse = -26'sd307923;
 9'd69: integrated_pulse = -26'sd318939;
 9'd70: integrated_pulse = -26'sd329145;
 9'd71: integrated_pulse = -26'sd338407;
 9'd72: integrated_pulse = -26'sd346589;
 9'd73: integrated_pulse = -26'sd353547;
 9'd74: integrated_pulse = -26'sd359136;
 9'd75: integrated_pulse = -26'sd363205;
 9'd76: integrated_pulse = -26'sd365603;
 9'd77: integrated_pulse = -26'sd366175;
 9'd78: integrated_pulse = -26'sd364764;
 9'd79: integrated_pulse = -26'sd361214;
 9'd80: integrated_pulse = -26'sd355366;
 9'd81: integrated_pulse = -26'sd347063;
 9'd82: integrated_pulse = -26'sd336149;
 9'd83: integrated_pulse = -26'sd322468;
 9'd84: integrated_pulse = -26'sd305869;
 9'd85: integrated_pulse = -26'sd286200;
 9'd86: integrated_pulse = -26'sd263317;
 9'd87: integrated_pulse = -26'sd237078;
 9'd88: integrated_pulse = -26'sd207347;
 9'd89: integrated_pulse = -26'sd173994;
 9'd90: integrated_pulse = -26'sd136894;
 9'd91: integrated_pulse = -26'sd95932;
 9'd92: integrated_pulse = -26'sd50999;
 9'd93: integrated_pulse = -26'sd1996;
 9'd94: integrated_pulse = 26'sd51169;
 9'd95: integrated_pulse = 26'sd108575;
 9'd96: integrated_pulse = 26'sd170294;
 9'd97: integrated_pulse = 26'sd236383;
 9'd98: integrated_pulse = 26'sd306893;
 9'd99: integrated_pulse = 26'sd381858;
 9'd100: integrated_pulse = 26'sd461302;
 9'd101: integrated_pulse = 26'sd545236;
 9'd102: integrated_pulse = 26'sd633659;
 9'd103: integrated_pulse = 26'sd726556;
 9'd104: integrated_pulse = 26'sd823899;
 9'd105: integrated_pulse = 26'sd925645;
 9'd106: integrated_pulse = 26'sd1031740;
 9'd107: integrated_pulse = 26'sd1142114;
 9'd108: integrated_pulse = 26'sd1256686;
 9'd109: integrated_pulse = 26'sd1375358;
 9'd110: integrated_pulse = 26'sd1498023;
 9'd111: integrated_pulse = 26'sd1624557;
 9'd112: integrated_pulse = 26'sd1754825;
 9'd113: integrated_pulse = 26'sd1888679;
 9'd114: integrated_pulse = 26'sd2025961;
 9'd115: integrated_pulse = 26'sd2166498;
 9'd116: integrated_pulse = 26'sd2310108;
 9'd117: integrated_pulse = 26'sd2456598;
 9'd118: integrated_pulse = 26'sd2605765;
 9'd119: integrated_pulse = 26'sd2757397;
 9'd120: integrated_pulse = 26'sd2911272;
 9'd121: integrated_pulse = 26'sd3067162;
 9'd122: integrated_pulse = 26'sd3224831;
 9'd123: integrated_pulse = 26'sd3384036;
 9'd124: integrated_pulse = 26'sd3544531;
 9'd125: integrated_pulse = 26'sd3706062;
 9'd126: integrated_pulse = 26'sd3868374;
 9'd127: integrated_pulse = 26'sd4031208;
 9'd128: integrated_pulse = 26'sd4194304;
 9'd129: integrated_pulse = 26'sd4357400;
 9'd130: integrated_pulse = 26'sd4520234;
 9'd131: integrated_pulse = 26'sd4682546;
 9'd132: integrated_pulse = 26'sd4844077;
 9'd133: integrated_pulse = 26'sd5004572;
 9'd134: integrated_pulse = 26'sd5163777;
 9'd135: integrated_pulse = 26'sd5321446;
 9'd136: integrated_pulse = 26'sd5477336;
 9'd137: integrated_pulse = 26'sd5631211;
 9'd138: integrated_pulse = 26'sd5782843;
 9'd139: integrated_pulse = 26'sd5932010;
 9'd140: integrated_pulse = 26'sd6078500;
 9'd141: integrated_pulse = 26'sd6222110;
 9'd142: integrated_pulse = 26'sd6362647;
 9'd143: integrated_pulse = 26'sd6499929;
 9'd144: integrated_pulse = 26'sd6633783;
 9'd145: integrated_pulse = 26'sd6764051;
 9'd146: integrated_pulse = 26'sd6890585;
 9'd147: integrated_pulse = 26'sd7013250;
 9'd148: integrated_pulse = 26'sd7131922;
 9'd149: integrated_pulse = 26'sd7246494;
 9'd150: integrated_pulse = 26'sd7356868;
 9'd151: integrated_pulse = 26'sd7462963;
 9'd152: integrated_pulse = 26'sd7564709;
 9'd153: integrated_pulse = 26'sd7662052;
 9'd154: integrated_pulse = 26'sd7754949;
 9'd155: integrated_pulse = 26'sd7843372;
 9'd156: integrated_pulse = 26'sd7927306;
 9'd157: integrated_pulse = 26'sd8006750;
 9'd158: integrated_pulse = 26'sd8081715;
 9'd159: integrated_pulse = 26'sd8152225;
 9'd160: integrated_pulse = 26'sd8218314;
 9'd161: integrated_pulse = 26'sd8280033;
 9'd162: integrated_pulse = 26'sd8337439;
 9'd163: integrated_pulse = 26'sd8390604;
 9'd164: integrated_pulse = 26'sd8439607;
 9'd165: integrated_pulse = 26'sd8484540;
 9'd166: integrated_pulse = 26'sd8525502;
 9'd167: integrated_pulse = 26'sd8562602;
 9'd168: integrated_pulse = 26'sd8595955;
 9'd169: integrated_pulse = 26'sd8625686;
 9'd170: integrated_pulse = 26'sd8651925;
 9'd171: integrated_pulse = 26'sd8674808;
 9'd172: integrated_pulse = 26'sd8694477;
 9'd173: integrated_pulse = 26'sd8711076;
 9'd174: integrated_pulse = 26'sd8724757;
 9'd175: integrated_pulse = 26'sd8735671;
 9'd176: integrated_pulse = 26'sd8743974;
 9'd177: integrated_pulse = 26'sd8749822;
 9'd178: integrated_pulse = 26'sd8753372;
 9'd179: integrated_pulse = 26'sd8754783;
 9'd180: integrated_pulse = 26'sd8754211;
 9'd181: integrated_pulse = 26'sd8751813;
 9'd182: integrated_pulse = 26'sd8747744;
 9'd183: integrated_pulse = 26'sd8742155;
 9'd184: integrated_pulse = 26'sd8735197;
 9'd185: integrated_pulse = 26'sd8727015;
 9'd186: integrated_pulse = 26'sd8717753;
 9'd187: integrated_pulse = 26'sd8707547;
 9'd188: integrated_pulse = 26'sd8696531;
 9'd189: integrated_pulse = 26'sd8684833;
 9'd190: integrated_pulse = 26'sd8672577;
 9'd191: integrated_pulse = 26'sd8659879;
 9'd192: integrated_pulse = 26'sd8646851;
 9'd193: integrated_pulse = 26'sd8633596;
 9'd194: integrated_pulse = 26'sd8620214;
 9'd195: integrated_pulse = 26'sd8606797;
 9'd196: integrated_pulse = 26'sd8593429;
 9'd197: integrated_pulse = 26'sd8580189;
 9'd198: integrated_pulse = 26'sd8567149;
 9'd199: integrated_pulse = 26'sd8554375;
 9'd200: integrated_pulse = 26'sd8541923;
 9'd201: integrated_pulse = 26'sd8529846;
 9'd202: integrated_pulse = 26'sd8518190;
 9'd203: integrated_pulse = 26'sd8506993;
 9'd204: integrated_pulse = 26'sd8496288;
 9'd205: integrated_pulse = 26'sd8486101;
 9'd206: integrated_pulse = 26'sd8476455;
 9'd207: integrated_pulse = 26'sd8467363;
 9'd208: integrated_pulse = 26'sd8458838;
 9'd209: integrated_pulse = 26'sd8450883;
 9'd210: integrated_pulse = 26'sd8443500;
 9'd211: integrated_pulse = 26'sd8436685;
 9'd212: integrated_pulse = 26'sd8430431;
 9'd213: integrated_pulse = 26'sd8424726;
 9'd214: integrated_pulse = 26'sd8419555;
 9'd215: integrated_pulse = 26'sd8414901;
 9'd216: integrated_pulse = 26'sd8410744;
 9'd217: integrated_pulse = 26'sd8407061;
 9'd218: integrated_pulse = 26'sd8403827;
 9'd219: integrated_pulse = 26'sd8401016;
 9'd220: integrated_pulse = 26'sd8398600;
 9'd221: integrated_pulse = 26'sd8396550;
 9'd222: integrated_pulse = 26'sd8394838;
 9'd223: integrated_pulse = 26'sd8393433;
 9'd224: integrated_pulse = 26'sd8392305;
 9'd225: integrated_pulse = 26'sd8391424;
 9'd226: integrated_pulse = 26'sd8390764;
 9'd227: integrated_pulse = 26'sd8390296;
 9'd228: integrated_pulse = 26'sd8389990;
 9'd229: integrated_pulse = 26'sd8389817;
 9'd230: integrated_pulse = 26'sd8389746;
 9'd231: integrated_pulse = 26'sd8389752;
 9'd232: integrated_pulse = 26'sd8389808;
 9'd233: integrated_pulse = 26'sd8389891;
 9'd234: integrated_pulse = 26'sd8389984;
 9'd235: integrated_pulse = 26'sd8390070;
 9'd236: integrated_pulse = 26'sd8390138;
 9'd237: integrated_pulse = 26'sd8390177;
 9'd238: integrated_pulse = 26'sd8390185;
 9'd239: integrated_pulse = 26'sd8390157;
 9'd240: integrated_pulse = 26'sd8390096;
 9'd241: integrated_pulse = 26'sd8390004;
 9'd242: integrated_pulse = 26'sd8389887;
 9'd243: integrated_pulse = 26'sd8389749;
 9'd244: integrated_pulse = 26'sd8389599;
 9'd245: integrated_pulse = 26'sd8389443;
 9'd246: integrated_pulse = 26'sd8389289;
 9'd247: integrated_pulse = 26'sd8389142;
 9'd248: integrated_pulse = 26'sd8389008;
 9'd249: integrated_pulse = 26'sd8388892;
 9'd250: integrated_pulse = 26'sd8388797;
 9'd251: integrated_pulse = 26'sd8388722;
 9'd252: integrated_pulse = 26'sd8388669;
 9'd253: integrated_pulse = 26'sd8388635;
 9'd254: integrated_pulse = 26'sd8388616;
 9'd255: integrated_pulse = 26'sd8388609;
 9'd256: integrated_pulse = 26'sd8388608;
 default: integrated_pulse = 26'sd8388608;
 endcase
 end
endfunction

reg [31:0] completed_area, completed_area_pipe;
reg signed [31:0] area_base, area_next, area_base_pipe, area_next_pipe;
reg [11:0] area_fraction_pipe;
reg [31:0] area_total, carrier_phase;
wire signed [32:0] area_difference =
 $signed({area_next_pipe[31],area_next_pipe})-$signed({area_base_pipe[31],area_base_pipe});
wire signed [45:0] area_interpolation = area_difference*$signed({1'b0,area_fraction_pipe});
integer area_tap;
always @* begin
 area_base=0;area_next=0;
 for(area_tap=0;area_tap<8;area_tap=area_tap+1) begin
  if(alpha_history[area_tap]==2'sd1) begin
   area_base=area_base+integrated_pulse(area_tap*32+bit_phase[31:27]);
   area_next=area_next+integrated_pulse(area_tap*32+bit_phase[31:27]+1);
  end else if(alpha_history[area_tap]==-2'sd1) begin
   area_base=area_base-integrated_pulse(area_tap*32+bit_phase[31:27]);
   area_next=area_next-integrated_pulse(area_tap*32+bit_phase[31:27]+1);
  end
 end
end

function signed [15:0] sine_lut;
    input [7:0] phase;
    reg [6:0] x;
    reg signed [15:0] y;
    begin
        case (phase[7:6])
            2'b00: x={1'b0,phase[5:0]}; 2'b01: x=7'd64-{1'b0,phase[5:0]};
            2'b10: x={1'b0,phase[5:0]}; default: x=7'd64-{1'b0,phase[5:0]};
        endcase
        case (x)
            7'd0:y=16'sd0;7'd1:y=16'sd804;7'd2:y=16'sd1608;7'd3:y=16'sd2410;
            7'd4:y=16'sd3212;7'd5:y=16'sd4011;7'd6:y=16'sd4808;7'd7:y=16'sd5602;
            7'd8:y=16'sd6393;7'd9:y=16'sd7179;7'd10:y=16'sd7962;7'd11:y=16'sd8739;
            7'd12:y=16'sd9512;7'd13:y=16'sd10278;7'd14:y=16'sd11039;7'd15:y=16'sd11793;
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
        sine_lut = phase[7] ? -y : y;
    end
endfunction

// Retain 16 phase bits instead of truncating the angle to eight bits.
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
        if (value > 32'sd2047) saturate_12=12'h7ff;
        else if (value < -32'sd2048) saturate_12=12'h800;
        else saturate_12=value[11:0];
    end
endfunction

always @* begin
    shaped_sum = 19'sd0;
    shaped_next = 19'sd0;
    for (k=0; k<8; k=k+1) begin
        if (alpha_history[k] == 2'sd1) begin
            shaped_sum = shaped_sum + frequency_pulse({1'b0,k[2:0],bit_phase[31:28]});
            shaped_next = shaped_next + frequency_pulse({1'b0,k[2:0],bit_phase[31:28]} + 8'd1);
        end else if (alpha_history[k] == -2'sd1) begin
            shaped_sum = shaped_sum - frequency_pulse({1'b0,k[2:0],bit_phase[31:28]});
            shaped_next = shaped_next - frequency_pulse({1'b0,k[2:0],bit_phase[31:28]} + 8'd1);
        end
    end
    // All taps share the fractional age: interpolate their sums only once.
    pulse_delta = $signed({shaped_next_pipe[18],shaped_next_pipe}) -
                  $signed({shaped_base_pipe[18],shaped_base_pipe});
    pulse_interpolation = pulse_delta * $signed({1'b0,pulse_fraction_pipe});
end

always @(posedge clk) begin
    if (rst || clear || !enable) begin
        completed_area<=0;completed_area_pipe<=0;area_base_pipe<=0;area_next_pipe<=0;
        area_fraction_pipe<=0;area_total<=0;carrier_phase<=0;
        have_bit <= 1'b0;
        bit_pair_phase <= 1'b0;
        delta_even <= 1'b0;
        delta_prev_odd <= 1'b0;
        symbol_parity <= 1'b0;
        a_prev1 <= -2'sd1;
        a_prev2 <= -2'sd1;
        shaped_sum_pipe <= 19'sd0;
        shaped_base_pipe <= 19'sd0;
        shaped_next_pipe <= 19'sd0;
        pulse_fraction_pipe <= 8'd0;
        phase_product <= 51'sd0;
        phase_acc <= 32'd0;
        accepted_bits <= 32'd0;
        debug_alpha <= 16'sd0;
        debug_frequency <= 16'sd0;
        debug_phase <= 32'd0;
        sine_i_pipe <= 16'sd0;
        sine_q_pipe <= 16'sd0;
        i_product <= 32'sd0;
        q_product <= 32'sd0;
        data_i <= 12'd0;
        data_q <= 12'd0;
        for (k=0; k<8; k=k+1)
            alpha_history[k] <= 2'sd0;
    end else begin
        if (bit_valid && bit_ready) begin
            have_bit <= 1'b1;
            accepted_bits <= accepted_bits + 1'b1;
            debug_alpha <= (alpha_next == 2'sd1) ? 16'sd32767 :
                           ((alpha_next == -2'sd1) ? -16'sd32767 : 16'sd0);
            for (k=7; k>0; k=k-1)
                alpha_history[k] <= alpha_history[k-1];
            alpha_history[0] <= alpha_next;
            completed_area <= completed_area + (alpha_history[7]==2'sd1 ? 32'd8388608 :
                alpha_history[7]==-2'sd1 ? -32'd8388608 : 32'd0);
            a_prev2 <= a_prev1;
            a_prev1 <= a_current;
            symbol_parity <= ~symbol_parity;
            if (differential_enable) begin
                if (!bit_pair_phase)
                    delta_even <= encoded_bit;
                else
                    delta_prev_odd <= encoded_bit;
                bit_pair_phase <= ~bit_pair_phase;
            end else begin
                bit_pair_phase <= ~bit_pair_phase;
            end
        end

        // Register both node sums and their matching fractional phase before
        // the interpolation multiply. This preserves sample alignment and
        // avoids an eight-tap sum plus multiply in one clock interval.
        shaped_base_pipe <= shaped_sum;
        shaped_next_pipe <= shaped_next;
        pulse_fraction_pipe <= bit_phase[27:20];
        shaped_sum_pipe <= shaped_base_pipe + ((pulse_interpolation + 29'sd128) >>> 8);
        if (shaped_sum_pipe > 19'sd32767) debug_frequency <= 16'sd32767;
        else if (shaped_sum_pipe < -19'sd32768) debug_frequency <= -16'sd32768;
        else debug_frequency <= shaped_sum_pipe[15:0];

        // Q15 pulse * bit_rate_word * h(=1/2) -> phase increment word.
        // shaped_sum_pipe is an explicit timing cut between the 8-way LUT
        // accumulation and the DSP multiplier at the 61.44 MHz sample clock.
        phase_product <= $signed(shaped_sum_pipe) * $signed({1'b0,bit_rate_word});
        area_base_pipe<=area_base;area_next_pipe<=area_next;
        completed_area_pipe<=completed_area;
        area_fraction_pipe<=bit_phase[26:15];
        area_total<=completed_area_pipe+area_base_pipe+((area_interpolation+46'sd2048)>>>12);
        carrier_phase<=carrier_phase+frequency_offset_word;
        // h=1/2: Q24 integrated area to Q32 phase cycles. No frequency
        // rectangle integration, hence no sample-ratio-dependent DC bias.
        phase_acc<=carrier_phase+(area_total<<7);
        debug_phase <= phase_acc;

        sine_i_pipe <= sine_interpolated(phase_acc[31:16] + 16'd16384);
        sine_q_pipe <= sine_interpolated(phase_acc[31:16]);
        i_product <= sine_i_pipe * $signed({1'b0,amplitude[14:0]});
        q_product <= sine_q_pipe * $signed({1'b0,amplitude[14:0]});
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

endmodule
