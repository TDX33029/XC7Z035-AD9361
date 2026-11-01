`timescale 1 ns / 1 ps

// IRIG-106 ARTM CPM complex-baseband transmitter.
//
// Processing chain:
//   MSB-first dibit mapper -> alternating h={4/16,5/16} ->
//   3-symbol temporal raised-cosine frequency pulse -> phase integrator -> I/Q.
//
// bit_rate_word = round(Rb / 61.44e6 * 2^32).  A symbol contains two bits,
// therefore Rs=Rb/2.  The 48-entry Q1.15 pulse table contains 16 samples per
// symbol and implements g(t)=[1-cos(2*pi*t/(3Ts))]/6 for 0<=t<3Ts.  Its
// discrete area is 1/2 to within coefficient quantisation error.
module artm_cpm_modulator (
    input  wire               clk,
    input  wire               rst,
    input  wire               enable,
    input  wire               clear,
    input  wire               bit_data,
    input  wire               bit_valid,
    output wire               bit_ready,
    input  wire [31:0]        bit_rate_word,
    input  wire [31:0]        bit_phase,
    input  wire signed [31:0] frequency_offset_word,
    input  wire [15:0]        amplitude,
    output reg  [11:0]        data_i,
    output reg  [11:0]        data_q,
    output wire               iq_valid,
    output reg  [31:0]        accepted_bits,
    output reg  signed [15:0] debug_symbol,
    output reg  signed [15:0] debug_frequency,
    output reg  [31:0]        debug_phase
);

reg have_first_bit;
reg first_bit;
reg have_symbol;
reg symbol_parity;
reg signed [5:0] beta_history [0:2];
// The accepted-bit parity supplies the half-symbol, while the scheduler
// supplies the fractional remainder. Never reset that remainder at a dibit.
wire [31:0] symbol_phase = {have_first_bit, bit_phase[31:1]};
reg signed [19:0] shaped_sum;
reg signed [19:0] shaped_sum_pipe;
reg signed [52:0] phase_product;
reg [31:0] phase_acc;
reg signed [31:0] i_product;
reg signed [31:0] q_product;

integer k;

assign bit_ready = enable && !rst;
assign iq_valid = enable && have_symbol && !rst;

wire [1:0] dibit = {first_bit, bit_data};
wire signed [3:0] alpha_next = (dibit == 2'b00) ? -4'sd3 :
                               (dibit == 2'b01) ? -4'sd1 :
                               (dibit == 2'b10) ?  4'sd1 : 4'sd3;
wire signed [5:0] beta_next = symbol_parity ?
                              ($signed(alpha_next) * 6'sd5) :
                              ($signed(alpha_next) * 6'sd4);

function signed [15:0] artm_pulse;
    input [5:0] index;
    begin
        case (index)
            6'd0:artm_pulse=16'sd0;       6'd1:artm_pulse=16'sd47;
            6'd2:artm_pulse=16'sd186;     6'd3:artm_pulse=16'sd416;
            6'd4:artm_pulse=16'sd732;     6'd5:artm_pulse=16'sd1129;
            6'd6:artm_pulse=16'sd1600;    6'd7:artm_pulse=16'sd2137;
            6'd8:artm_pulse=16'sd2731;    6'd9:artm_pulse=16'sd3371;
            6'd10:artm_pulse=16'sd4048;   6'd11:artm_pulse=16'sd4748;
            6'd12:artm_pulse=16'sd5461;   6'd13:artm_pulse=16'sd6174;
            6'd14:artm_pulse=16'sd6875;   6'd15:artm_pulse=16'sd7551;
            6'd16:artm_pulse=16'sd8192;   6'd17:artm_pulse=16'sd8786;
            6'd18:artm_pulse=16'sd9323;   6'd19:artm_pulse=16'sd9794;
            6'd20:artm_pulse=16'sd10191;  6'd21:artm_pulse=16'sd10507;
            6'd22:artm_pulse=16'sd10737;  6'd23:artm_pulse=16'sd10876;
            6'd24:artm_pulse=16'sd10923;  6'd25:artm_pulse=16'sd10876;
            6'd26:artm_pulse=16'sd10737;  6'd27:artm_pulse=16'sd10507;
            6'd28:artm_pulse=16'sd10191;  6'd29:artm_pulse=16'sd9794;
            6'd30:artm_pulse=16'sd9323;   6'd31:artm_pulse=16'sd8786;
            6'd32:artm_pulse=16'sd8192;   6'd33:artm_pulse=16'sd7551;
            6'd34:artm_pulse=16'sd6875;   6'd35:artm_pulse=16'sd6174;
            6'd36:artm_pulse=16'sd5461;   6'd37:artm_pulse=16'sd4748;
            6'd38:artm_pulse=16'sd4048;   6'd39:artm_pulse=16'sd3371;
            6'd40:artm_pulse=16'sd2731;   6'd41:artm_pulse=16'sd2137;
            6'd42:artm_pulse=16'sd1600;   6'd43:artm_pulse=16'sd1129;
            6'd44:artm_pulse=16'sd732;    6'd45:artm_pulse=16'sd416;
            6'd46:artm_pulse=16'sd186;    6'd47:artm_pulse=16'sd47;
            default:artm_pulse=16'sd0;
        endcase
    end
endfunction

function signed [15:0] sine_lut;
    input [7:0] phase;
    reg [6:0] x;
    reg signed [15:0] y;
    begin
        case (phase[7:6])
            2'b00:x={1'b0,phase[5:0]}; 2'b01:x=7'd64-{1'b0,phase[5:0]};
            2'b10:x={1'b0,phase[5:0]}; default:x=7'd64-{1'b0,phase[5:0]};
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

function [11:0] saturate_12;
    input signed [31:0] value;
    begin
        if (value > 32'sd2047) saturate_12=12'h7ff;
        else if (value < -32'sd2048) saturate_12=12'h800;
        else saturate_12=value[11:0];
    end
endfunction

always @* begin
    shaped_sum = 20'sd0;
    for (k=0; k<3; k=k+1)
        shaped_sum = shaped_sum + beta_history[k] *
                     artm_pulse({k[1:0],symbol_phase[31:28]});
end

always @(posedge clk) begin
    if (rst || clear || !enable) begin
        have_first_bit <= 1'b0;
        first_bit <= 1'b0;
        have_symbol <= 1'b0;
        symbol_parity <= 1'b0;
        shaped_sum_pipe <= 20'sd0;
        phase_product <= 53'sd0;
        phase_acc <= 32'd0;
        i_product <= 32'sd0;
        q_product <= 32'sd0;
        data_i <= 12'd0;
        data_q <= 12'd0;
        accepted_bits <= 32'd0;
        debug_symbol <= 16'sd0;
        debug_frequency <= 16'sd0;
        debug_phase <= 32'd0;
        for (k=0; k<3; k=k+1)
            beta_history[k] <= 6'sd0;
    end else begin
        if (bit_valid && bit_ready) begin
            accepted_bits <= accepted_bits + 1'b1;
            if (!have_first_bit) begin
                first_bit <= bit_data;
                have_first_bit <= 1'b1;
            end else begin
                have_first_bit <= 1'b0;
                have_symbol <= 1'b1;
                debug_symbol <= {{12{alpha_next[3]}},alpha_next};
                beta_history[2] <= beta_history[1];
                beta_history[1] <= beta_history[0];
                beta_history[0] <= beta_next;
                symbol_parity <= ~symbol_parity;
            end
        end

        shaped_sum_pipe <= shaped_sum;
        if (shaped_sum_pipe > 20'sd32767) debug_frequency <= 16'sd32767;
        else if (shaped_sum_pipe < -20'sd32768) debug_frequency <= -16'sd32768;
        else debug_frequency <= shaped_sum_pipe[15:0];

        // beta=alpha*h_num, h=h_num/16, Rs=Rb/2, pulse is Q1.15.
        // The total divisor is 16*2*2^15 = 2^20.
        phase_product <= $signed(shaped_sum_pipe) * $signed({1'b0,bit_rate_word});
        phase_acc <= phase_acc + frequency_offset_word + (phase_product >>> 20);
        debug_phase <= phase_acc;

        i_product <= sine_lut(phase_acc[31:24] + 8'd64) *
                     $signed({1'b0,amplitude[14:0]});
        q_product <= sine_lut(phase_acc[31:24]) *
                     $signed({1'b0,amplitude[14:0]});
        if (have_symbol) begin
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
