//////////////////////////////////////////////////////////////////////////////////
// AD9361 TX waveform source, integrated from the board vendor's AD9361_FM and
// AD9361_QPSK examples.  The existing AXI-Lite/Block Design interface is kept:
//
//   phase_inc = 0 : OFF (zero I/Q)
//   phase_inc = 1 : QPSK, 400 ksym/s, 0xFFF5AFFF header + incrementing payload
//   phase_inc = 2 : FM, 2 MHz complex IF, 20 kHz tone, vendor deviation setting
//   6             : explicit 2 MHz complex sine/CW test source
//
// Constants assume the 61.44 MHz I/Q sample clock exported by the LVDS PHY.
//////////////////////////////////////////////////////////////////////////////////
module tx_waveform_channel (
	input  wire        clk,
	input  wire        rst,
	input  wire [3:0]  mode,
	input  wire [2:0]  rolloff,
	output wire        dac_valid,
	output reg  [11:0] dac_data_i0,
	output reg  [11:0] dac_data_q0,
	output reg  [11:0] cw_data_i,
	output reg  [11:0] cw_data_q
);

localparam [3:0] TX_OFF  = 4'd0;
localparam [3:0] TX_QPSK = 4'd1;
localparam [3:0] TX_FM   = 4'd2;
localparam [3:0] TX_CW   = 4'd6;

localparam [31:0] FM_CARRIER_WORD = 32'd139810133; // 2 MHz at 61.44 MSPS
localparam [31:0] FM_MESSAGE_WORD = 32'd1398101;   // 20 kHz at 61.44 MSPS
localparam signed [31:0] FM_OFFSET_WORD = 32'sd13981013; // 200 kHz deviation
// 61.44M/154 = 398.96 ksym/s.  Exact profile-specific symbol timing will be
// supplied by the later PCM/FM and SOQPSK fractional-rate timing engine.
localparam integer QPSK_CLOCKS_PER_SYMBOL = 154;
localparam signed [15:0] QPSK_LEVEL = 16'sd23169;

reg [3:0] control_meta;
reg [3:0] control_sync;
reg [3:0] control_last;
reg [2:0] rolloff_meta;
reg [2:0] rolloff_sync;
reg [2:0] rolloff_last;

reg [31:0] cw_phase;
reg signed [25:0] cw_interp_i, cw_interp_q;
reg [31:0] fm_phase;
reg [31:0] fm_message_phase;
wire signed [15:0] fm_message;
wire signed [47:0] fm_product;
wire signed [31:0] fm_delta;
wire [31:0] fm_step;

reg [7:0] qpsk_clock_count;
reg [3:0] qpsk_symbol_index;
reg       qpsk_header_active;
reg [31:0] qpsk_payload;
reg [31:0] qpsk_word;
reg signed [15:0] qpsk_raw_i;
reg signed [15:0] qpsk_raw_q;
reg signed [15:0] fir_i_delay [0:20];
reg signed [15:0] fir_q_delay [0:20];
reg signed [31:0] fir_i_product [0:10];
reg signed [31:0] fir_q_product [0:10];
reg signed [31:0] fir_i_sum_l1 [0:5];
reg signed [31:0] fir_q_sum_l1 [0:5];
reg signed [31:0] fir_i_sum_l2 [0:2];
reg signed [31:0] fir_q_sum_l2 [0:2];
reg signed [31:0] fir_i_sum_l3 [0:1];
reg signed [31:0] fir_q_sum_l3 [0:1];
reg signed [31:0] fir_i_total;
reg signed [31:0] fir_q_total;

integer tap;

// Quarter-wave LUT.  The vendor example uses a 4096x16 ROM; this symmetric
// 256-phase implementation preserves its signed amplitude and frequency words
// while avoiding three additional generated-IP dependencies in the merged BD.
function signed [15:0] quarter_sine;
	input [6:0] index;
	begin
		case (index)
			7'd0: quarter_sine = 16'sd0;
			7'd1: quarter_sine = 16'sd804;
			7'd2: quarter_sine = 16'sd1608;
			7'd3: quarter_sine = 16'sd2410;
			7'd4: quarter_sine = 16'sd3212;
			7'd5: quarter_sine = 16'sd4011;
			7'd6: quarter_sine = 16'sd4808;
			7'd7: quarter_sine = 16'sd5602;
			7'd8: quarter_sine = 16'sd6393;
			7'd9: quarter_sine = 16'sd7179;
			7'd10: quarter_sine = 16'sd7962;
			7'd11: quarter_sine = 16'sd8739;
			7'd12: quarter_sine = 16'sd9512;
			7'd13: quarter_sine = 16'sd10278;
			7'd14: quarter_sine = 16'sd11039;
			7'd15: quarter_sine = 16'sd11793;
			7'd16: quarter_sine = 16'sd12539;
			7'd17: quarter_sine = 16'sd13279;
			7'd18: quarter_sine = 16'sd14010;
			7'd19: quarter_sine = 16'sd14732;
			7'd20: quarter_sine = 16'sd15446;
			7'd21: quarter_sine = 16'sd16151;
			7'd22: quarter_sine = 16'sd16846;
			7'd23: quarter_sine = 16'sd17530;
			7'd24: quarter_sine = 16'sd18204;
			7'd25: quarter_sine = 16'sd18868;
			7'd26: quarter_sine = 16'sd19519;
			7'd27: quarter_sine = 16'sd20159;
			7'd28: quarter_sine = 16'sd20787;
			7'd29: quarter_sine = 16'sd21403;
			7'd30: quarter_sine = 16'sd22005;
			7'd31: quarter_sine = 16'sd22594;
			7'd32: quarter_sine = 16'sd23170;
			7'd33: quarter_sine = 16'sd23731;
			7'd34: quarter_sine = 16'sd24279;
			7'd35: quarter_sine = 16'sd24811;
			7'd36: quarter_sine = 16'sd25329;
			7'd37: quarter_sine = 16'sd25832;
			7'd38: quarter_sine = 16'sd26319;
			7'd39: quarter_sine = 16'sd26790;
			7'd40: quarter_sine = 16'sd27245;
			7'd41: quarter_sine = 16'sd27683;
			7'd42: quarter_sine = 16'sd28105;
			7'd43: quarter_sine = 16'sd28510;
			7'd44: quarter_sine = 16'sd28898;
			7'd45: quarter_sine = 16'sd29268;
			7'd46: quarter_sine = 16'sd29621;
			7'd47: quarter_sine = 16'sd29956;
			7'd48: quarter_sine = 16'sd30273;
			7'd49: quarter_sine = 16'sd30571;
			7'd50: quarter_sine = 16'sd30852;
			7'd51: quarter_sine = 16'sd31113;
			7'd52: quarter_sine = 16'sd31356;
			7'd53: quarter_sine = 16'sd31580;
			7'd54: quarter_sine = 16'sd31785;
			7'd55: quarter_sine = 16'sd31971;
			7'd56: quarter_sine = 16'sd32137;
			7'd57: quarter_sine = 16'sd32285;
			7'd58: quarter_sine = 16'sd32412;
			7'd59: quarter_sine = 16'sd32521;
			7'd60: quarter_sine = 16'sd32609;
			7'd61: quarter_sine = 16'sd32678;
			7'd62: quarter_sine = 16'sd32728;
			7'd63: quarter_sine = 16'sd32757;
			default: quarter_sine = 16'sd32767;
		endcase
	end
endfunction

function signed [15:0] sine_lut;
	input [7:0] phase;
	reg [5:0] offset;
	reg [6:0] mirror_index;
	reg signed [15:0] magnitude;
	begin
		offset = phase[5:0];
		mirror_index = 7'd64 - {1'b0, offset};
		case (phase[7:6])
			2'b00: begin magnitude = quarter_sine({1'b0, offset}); sine_lut = magnitude; end
			2'b01: begin magnitude = quarter_sine(mirror_index); sine_lut = magnitude; end
			2'b10: begin magnitude = quarter_sine({1'b0, offset}); sine_lut = -magnitude; end
			default: begin magnitude = quarter_sine(mirror_index); sine_lut = -magnitude; end
		endcase
	end
endfunction

// CW only: retain eight more phase bits and interpolate neighbouring LUT
// entries.  The old 8-bit phase truncation produced prominent aliased phase
// spurs at -18.48 / +22.48 MHz for the fixed 2 MHz tone at 61.44 MSPS.
// Symmetric rounding/clamping avoids the -0.5 LSB DC bias of >>> 4.
// No interface/register change; legacy FM/QPSK and telemetry paths unchanged.
function signed [25:0] cw_interpolate;
	input [15:0] phase;
	reg signed [15:0] y0, y1;
	reg signed [16:0] delta;
	reg signed [25:0] product;
	begin
		y0 = sine_lut(phase[15:8]);
		y1 = sine_lut(phase[15:8] + 8'd1);
		delta = $signed({y1[15], y1}) - $signed({y0[15], y0});
		product = delta * $signed({1'b0, phase[7:0]});
		cw_interpolate = $signed({{2{y0[15]}}, y0, 8'd0}) + product;
	end
endfunction

// Separate register stage before sign/round/saturation keeps the multiplier
// and the final rounding carry chain out of the same 61.44 MHz timing path.
function [11:0] cw_round_12;
	input signed [25:0] weighted;
	reg signed [25:0] rounded;
	begin
		if (weighted < 0)
			rounded = -(((-weighted) + 26'sd2048) >>> 12);
		else
			rounded = (weighted + 26'sd2048) >>> 12;
		if (rounded > 26'sd2047) cw_round_12 = 12'sd2047;
		else if (rounded < -26'sd2047) cw_round_12 = -12'sd2047;
		else cw_round_12 = rounded[11:0];
	end
endfunction

function signed [16:0] add_symmetric;
	input signed [15:0] a;
	input signed [15:0] b;
	begin
		add_symmetric = $signed({a[15], a}) + $signed({b[15], b});
	end
endfunction

function [11:0] saturate_12;
	input signed [31:0] value;
	begin
		if (value > 32'sd2047)
			saturate_12 = 12'h7FF;
		else if (value < -32'sd2048)
			saturate_12 = 12'h800;
		else
			saturate_12 = value[11:0];
	end
endfunction

// Legacy QPSK diagnostic FIR presets.  The standard PL telemetry waveforms
// (PCM/FM and SOQPSK-TG) do not use this path.  Code 0 is the
// historical coefficient set; codes 1..5 provide the common operator labels
// alpha=0.20, 0.35, 0.50, 0.75 and 1.00.  They are intentionally kept as
// compact integer presets so changing the selector does not alter the BD
// interface or require a coefficient DMA path.
function signed [7:0] qpsk_coeff;
	input [3:0] index;
	input [2:0] alpha;
	begin
		case (alpha)
			3'd1: case (index)
				4'd0:qpsk_coeff=8'sd4;  4'd1:qpsk_coeff=8'sd0;
				4'd2:qpsk_coeff=-8'sd3; 4'd3:qpsk_coeff=-8'sd2;
				4'd4:qpsk_coeff=8'sd3;  4'd5:qpsk_coeff=8'sd5;
				4'd6:qpsk_coeff=-8'sd5; 4'd7:qpsk_coeff=-8'sd10;
				4'd8:qpsk_coeff=8'sd6;  4'd9:qpsk_coeff=8'sd40;
				default:qpsk_coeff=8'sd62;
			endcase
		3'd3: case (index)
				4'd0:qpsk_coeff=8'sd8;  4'd1:qpsk_coeff=8'sd0;
				4'd2:qpsk_coeff=-8'sd5; 4'd3:qpsk_coeff=-8'sd4;
				4'd4:qpsk_coeff=8'sd6;  4'd5:qpsk_coeff=8'sd7;
				4'd6:qpsk_coeff=-8'sd8; 4'd7:qpsk_coeff=-8'sd16;
				4'd8:qpsk_coeff=8'sd8;  4'd9:qpsk_coeff=8'sd48;
				default:qpsk_coeff=8'sd66;
			endcase
		3'd4: case (index)
				4'd0:qpsk_coeff=8'sd10; 4'd1:qpsk_coeff=8'sd0;
				4'd2:qpsk_coeff=-8'sd6; 4'd3:qpsk_coeff=-8'sd5;
				4'd4:qpsk_coeff=8'sd7;  4'd5:qpsk_coeff=8'sd9;
				4'd6:qpsk_coeff=-8'sd10;4'd7:qpsk_coeff=-8'sd20;
				4'd8:qpsk_coeff=8'sd10; 4'd9:qpsk_coeff=8'sd52;
				default:qpsk_coeff=8'sd70;
			endcase
		3'd5: case (index)
				4'd0:qpsk_coeff=8'sd12; 4'd1:qpsk_coeff=8'sd0;
				4'd2:qpsk_coeff=-8'sd8; 4'd3:qpsk_coeff=-8'sd6;
				4'd4:qpsk_coeff=8'sd9;  4'd5:qpsk_coeff=8'sd11;
				4'd6:qpsk_coeff=-8'sd12;4'd7:qpsk_coeff=-8'sd24;
				4'd8:qpsk_coeff=8'sd12; 4'd9:qpsk_coeff=8'sd56;
				default:qpsk_coeff=8'sd74;
			endcase
		default: case (index)
				4'd0:qpsk_coeff=8'sd6;  4'd1:qpsk_coeff=8'sd0;
				4'd2:qpsk_coeff=-8'sd4; 4'd3:qpsk_coeff=-8'sd3;
				4'd4:qpsk_coeff=8'sd5;  4'd5:qpsk_coeff=8'sd6;
				4'd6:qpsk_coeff=-8'sd6; 4'd7:qpsk_coeff=-8'sd13;
				4'd8:qpsk_coeff=8'sd7;  4'd9:qpsk_coeff=8'sd44;
				default:qpsk_coeff=8'sd64;
			endcase
		endcase
	end
endfunction

assign fm_message = sine_lut(fm_message_phase[31:24]);
assign fm_product = fm_message * FM_OFFSET_WORD;
assign fm_delta = fm_product >>> 16;
assign fm_step = FM_CARRIER_WORD + fm_delta;
assign dac_valid = ~rst;

always @(posedge clk or posedge rst) begin
	if (rst) begin
		control_meta       <= TX_OFF;
		control_sync       <= TX_OFF;
		control_last       <= TX_OFF;
		rolloff_meta       <= 3'd0;
		rolloff_sync       <= 3'd0;
		rolloff_last       <= 3'd0;
		cw_phase           <= 32'd0;
		cw_interp_i        <= 26'sd0;
		cw_interp_q        <= 26'sd0;
		fm_phase           <= 32'd0;
		fm_message_phase   <= 32'd0;
		qpsk_clock_count   <= 8'd0;
		qpsk_symbol_index  <= 4'd0;
		qpsk_header_active <= 1'b1;
		qpsk_payload       <= 32'd0;
		qpsk_word          <= 32'hFFF5AFFF;
		qpsk_raw_i         <= QPSK_LEVEL;
		qpsk_raw_q         <= QPSK_LEVEL;
		dac_data_i0        <= 12'd0;
		dac_data_q0        <= 12'd0;
		cw_data_i         <= 12'd0;
		cw_data_q         <= 12'd0;
		for (tap = 0; tap < 21; tap = tap + 1) begin
			fir_i_delay[tap] <= 16'sd0;
			fir_q_delay[tap] <= 16'sd0;
		end
		for (tap = 0; tap < 11; tap = tap + 1) begin
			fir_i_product[tap] <= 32'sd0;
			fir_q_product[tap] <= 32'sd0;
		end
		for (tap = 0; tap < 6; tap = tap + 1) begin
			fir_i_sum_l1[tap] <= 32'sd0;
			fir_q_sum_l1[tap] <= 32'sd0;
		end
		for (tap = 0; tap < 3; tap = tap + 1) begin
			fir_i_sum_l2[tap] <= 32'sd0;
			fir_q_sum_l2[tap] <= 32'sd0;
		end
		fir_i_sum_l3[0] <= 32'sd0; fir_i_sum_l3[1] <= 32'sd0;
		fir_q_sum_l3[0] <= 32'sd0; fir_q_sum_l3[1] <= 32'sd0;
		fir_i_total <= 32'sd0;
		fir_q_total <= 32'sd0;
	end else begin
		// Static multi-bit control crosses from AXI clock; two stages are enough
		// because software leaves it unchanged between mode commands.
		control_meta <= mode;
		control_sync <= control_meta;
		rolloff_meta <= rolloff;
		rolloff_sync <= rolloff_meta;

		if (control_sync != control_last || rolloff_sync != rolloff_last) begin
			control_last       <= control_sync;
			rolloff_last       <= rolloff_sync;
			cw_phase           <= 32'd0;
			cw_interp_i        <= 26'sd0;
			cw_interp_q        <= 26'sd0;
			fm_phase           <= 32'd0;
			fm_message_phase   <= 32'd0;
			qpsk_clock_count   <= 8'd0;
			qpsk_symbol_index  <= 4'd0;
			qpsk_header_active <= 1'b1;
			qpsk_payload       <= 32'd0;
			qpsk_word          <= 32'hFFF5AFFF;
			qpsk_raw_i         <= QPSK_LEVEL;
			qpsk_raw_q         <= QPSK_LEVEL;
			for (tap = 0; tap < 21; tap = tap + 1) begin
				fir_i_delay[tap] <= 16'sd0;
				fir_q_delay[tap] <= 16'sd0;
			end
			for (tap = 0; tap < 11; tap = tap + 1) begin
				fir_i_product[tap] <= 32'sd0;
				fir_q_product[tap] <= 32'sd0;
			end
			for (tap = 0; tap < 6; tap = tap + 1) begin
				fir_i_sum_l1[tap] <= 32'sd0;
				fir_q_sum_l1[tap] <= 32'sd0;
			end
			for (tap = 0; tap < 3; tap = tap + 1) begin
				fir_i_sum_l2[tap] <= 32'sd0;
				fir_q_sum_l2[tap] <= 32'sd0;
			end
			fir_i_sum_l3[0] <= 32'sd0; fir_i_sum_l3[1] <= 32'sd0;
			fir_q_sum_l3[0] <= 32'sd0; fir_q_sum_l3[1] <= 32'sd0;
			fir_i_total <= 32'sd0;
			fir_q_total <= 32'sd0;
			cw_data_i <= 12'd0;
			cw_data_q <= 12'd0;
		end

		case (control_sync)
			TX_OFF: begin
				dac_data_i0 <= 12'd0;
				dac_data_q0 <= 12'd0;
			end

			TX_QPSK: begin
				// Symmetric 21-tap FIR in a five-stage pipeline.  This implements
				// the vendor coefficients without a single-cycle DSP cascade.
				fir_i_product[0]  <= add_symmetric(fir_i_delay[0], fir_i_delay[20]) * qpsk_coeff(4'd0, rolloff_sync);
				fir_i_product[1]  <= 32'sd0;
				fir_i_product[2]  <= add_symmetric(fir_i_delay[2], fir_i_delay[18]) * qpsk_coeff(4'd2, rolloff_sync);
				fir_i_product[3]  <= add_symmetric(fir_i_delay[3], fir_i_delay[17]) * qpsk_coeff(4'd3, rolloff_sync);
				fir_i_product[4]  <= add_symmetric(fir_i_delay[4], fir_i_delay[16]) * qpsk_coeff(4'd4, rolloff_sync);
				fir_i_product[5]  <= add_symmetric(fir_i_delay[5], fir_i_delay[15]) * qpsk_coeff(4'd5, rolloff_sync);
				fir_i_product[6]  <= add_symmetric(fir_i_delay[6], fir_i_delay[14]) * qpsk_coeff(4'd6, rolloff_sync);
				fir_i_product[7]  <= add_symmetric(fir_i_delay[7], fir_i_delay[13]) * qpsk_coeff(4'd7, rolloff_sync);
				fir_i_product[8]  <= add_symmetric(fir_i_delay[8], fir_i_delay[12]) * qpsk_coeff(4'd8, rolloff_sync);
				fir_i_product[9]  <= add_symmetric(fir_i_delay[9], fir_i_delay[11]) * qpsk_coeff(4'd9, rolloff_sync);
				fir_i_product[10] <= $signed(fir_i_delay[10]) * qpsk_coeff(4'd10, rolloff_sync);
				fir_q_product[0]  <= add_symmetric(fir_q_delay[0], fir_q_delay[20]) * qpsk_coeff(4'd0, rolloff_sync);
				fir_q_product[1]  <= 32'sd0;
				fir_q_product[2]  <= add_symmetric(fir_q_delay[2], fir_q_delay[18]) * qpsk_coeff(4'd2, rolloff_sync);
				fir_q_product[3]  <= add_symmetric(fir_q_delay[3], fir_q_delay[17]) * qpsk_coeff(4'd3, rolloff_sync);
				fir_q_product[4]  <= add_symmetric(fir_q_delay[4], fir_q_delay[16]) * qpsk_coeff(4'd4, rolloff_sync);
				fir_q_product[5]  <= add_symmetric(fir_q_delay[5], fir_q_delay[15]) * qpsk_coeff(4'd5, rolloff_sync);
				fir_q_product[6]  <= add_symmetric(fir_q_delay[6], fir_q_delay[14]) * qpsk_coeff(4'd6, rolloff_sync);
				fir_q_product[7]  <= add_symmetric(fir_q_delay[7], fir_q_delay[13]) * qpsk_coeff(4'd7, rolloff_sync);
				fir_q_product[8]  <= add_symmetric(fir_q_delay[8], fir_q_delay[12]) * qpsk_coeff(4'd8, rolloff_sync);
				fir_q_product[9]  <= add_symmetric(fir_q_delay[9], fir_q_delay[11]) * qpsk_coeff(4'd9, rolloff_sync);
				fir_q_product[10] <= $signed(fir_q_delay[10]) * qpsk_coeff(4'd10, rolloff_sync);

				for (tap = 0; tap < 5; tap = tap + 1) begin
					fir_i_sum_l1[tap] <= fir_i_product[tap*2] + fir_i_product[tap*2+1];
					fir_q_sum_l1[tap] <= fir_q_product[tap*2] + fir_q_product[tap*2+1];
				end
				fir_i_sum_l1[5] <= fir_i_product[10];
				fir_q_sum_l1[5] <= fir_q_product[10];
				fir_i_sum_l2[0] <= fir_i_sum_l1[0] + fir_i_sum_l1[1];
				fir_i_sum_l2[1] <= fir_i_sum_l1[2] + fir_i_sum_l1[3];
				fir_i_sum_l2[2] <= fir_i_sum_l1[4] + fir_i_sum_l1[5];
				fir_q_sum_l2[0] <= fir_q_sum_l1[0] + fir_q_sum_l1[1];
				fir_q_sum_l2[1] <= fir_q_sum_l1[2] + fir_q_sum_l1[3];
				fir_q_sum_l2[2] <= fir_q_sum_l1[4] + fir_q_sum_l1[5];
				fir_i_sum_l3[0] <= fir_i_sum_l2[0] + fir_i_sum_l2[1];
				fir_i_sum_l3[1] <= fir_i_sum_l2[2];
				fir_q_sum_l3[0] <= fir_q_sum_l2[0] + fir_q_sum_l2[1];
				fir_q_sum_l3[1] <= fir_q_sum_l2[2];
				fir_i_total <= fir_i_sum_l3[0] + fir_i_sum_l3[1];
				fir_q_total <= fir_q_sum_l3[0] + fir_q_sum_l3[1];
				dac_data_i0 <= saturate_12(fir_i_total >>> 11);
				dac_data_q0 <= saturate_12(fir_q_total >>> 11);
				for (tap = 20; tap > 0; tap = tap - 1) begin
					fir_i_delay[tap] <= fir_i_delay[tap-1];
					fir_q_delay[tap] <= fir_q_delay[tap-1];
				end
				fir_i_delay[0] <= qpsk_raw_i;
				fir_q_delay[0] <= qpsk_raw_q;

				if (qpsk_clock_count == QPSK_CLOCKS_PER_SYMBOL - 1) begin
					qpsk_clock_count <= 8'd0;
					case ({qpsk_word[31-(qpsk_symbol_index*2)],
					       qpsk_word[30-(qpsk_symbol_index*2)]})
						2'b00: begin qpsk_raw_i <=  QPSK_LEVEL; qpsk_raw_q <=  QPSK_LEVEL; end
						2'b01: begin qpsk_raw_i <= -QPSK_LEVEL; qpsk_raw_q <=  QPSK_LEVEL; end
						2'b11: begin qpsk_raw_i <= -QPSK_LEVEL; qpsk_raw_q <= -QPSK_LEVEL; end
						default: begin qpsk_raw_i <= QPSK_LEVEL; qpsk_raw_q <= -QPSK_LEVEL; end
					endcase
					if (qpsk_symbol_index == 4'd15) begin
						qpsk_symbol_index <= 4'd0;
						if (qpsk_header_active) begin
							qpsk_header_active <= 1'b0;
							qpsk_word <= qpsk_payload;
						end else begin
							qpsk_payload <= qpsk_payload + 32'd1;
							qpsk_word <= qpsk_payload + 32'd1;
						end
					end else begin
						qpsk_symbol_index <= qpsk_symbol_index + 4'd1;
					end
				end else begin
					qpsk_clock_count <= qpsk_clock_count + 8'd1;
				end
			end

			TX_FM: begin
				fm_message_phase <= fm_message_phase + FM_MESSAGE_WORD;
				fm_phase <= fm_phase + fm_step;
				dac_data_i0 <= sine_lut(fm_phase[31:24] + 8'd64) >>> 4;
				dac_data_q0 <= sine_lut(fm_phase[31:24]) >>> 4;
			end

			TX_CW: begin
				/* Explicit complex sine source for RF/link checks.  I and Q
				 * are in quadrature; the fixed 2 MHz phase word places the
				 * tone at LO+2 MHz after AD9361 upconversion. */
				cw_phase <= cw_phase + FM_CARRIER_WORD;
				cw_interp_i <= cw_interpolate(cw_phase[31:16] + 16'd16384);
				cw_interp_q <= cw_interpolate(cw_phase[31:16]);
				cw_data_i <= cw_round_12(cw_interp_i);
				cw_data_q <= cw_round_12(cw_interp_q);
			end

			default: begin
				/* Undefined modes are silent; CW is mode 6 and is selected
				 * explicitly by tx_waveform_mux. */
				dac_data_i0 <= 12'd0;
				dac_data_q0 <= 12'd0;
			end
		endcase
	end
end

endmodule

/*
 * Explicit, fail-safe waveform selector.  Keep this boundary independent of
 * the individual modulators so later IRIG waveforms can be added as another
 * source port and one case item.  Undefined mode values produce zero I/Q;
 * they must never fall through to the legacy diagnostic CW source.
 */
module tx_waveform_mux (
	input  wire [3:0]  mode,
	input  wire [11:0] legacy_i,
	input  wire [11:0] legacy_q,
	input  wire [11:0] cw_i,
	input  wire [11:0] cw_q,
	input  wire [11:0] soqpsk_i,
	input  wire [11:0] soqpsk_q,
	input  wire [11:0] pcm_i,
	input  wire [11:0] pcm_q,
	output reg  [11:0] selected_i,
	output reg  [11:0] selected_q
);
	always @* begin
		selected_i = 12'd0;
		selected_q = 12'd0;
		case (mode)
			4'd0: begin selected_i = 12'd0;     selected_q = 12'd0;     end
			4'd1,
			4'd2: begin selected_i = legacy_i;  selected_q = legacy_q;  end
			4'd6: begin selected_i = cw_i;      selected_q = cw_q;      end
			4'd3: begin selected_i = soqpsk_i;  selected_q = soqpsk_q;  end
			4'd4: begin selected_i = pcm_i;     selected_q = pcm_q;     end
			default: begin selected_i = 12'd0;  selected_q = 12'd0;     end
		endcase
	end
endmodule

/*
 * Dual TX wrapper.
 *
 * phase_inc[3:0]  = TX1 mode, phase_inc[11:8] = TX2 mode.
 * Mode 3 selects SOQPSK-TG, mode 4 selects PCM/FM and mode 6 selects
 * the explicit complex sine/CW test source. Mode 5 is intentionally silent.
 * A selected bit-driven waveform can be routed to TX1,
 * TX2 or both while the other channel remains available for a legacy
 * QPSK/FM diagnostic waveform.  Different bit-driven modes cannot be mixed.
 */
module dds_inc (
	input  wire        clk,
	input  wire        rst,
	input  wire [31:0] phase_inc,
	input  wire        bit_data,
	input  wire        bit_valid,
	input  wire [31:0] bit_phase,
	input  wire        frame_start,
	input  wire        frame_end,
	output wire        bit_ready,
	input  wire [31:0] pcm_deviation_word,
	input  wire [31:0] pcm_frequency_offset,
	input  wire [15:0] pcm_amplitude,
	input  wire [3:0]  pcm_filter_profile,
	input  wire [31:0] pcm_control,
	input  wire [31:0] soqpsk_bit_rate_word,
	input  wire [31:0] artm_bit_rate_word, // retired BD port; retained for IP compatibility
	output wire        pcm_iq_valid,
	output wire [31:0] pcm_accepted_bits,
	output wire signed [15:0] pcm_debug_nrz,
	output wire signed [15:0] pcm_debug_filter,
	output wire [31:0] pcm_debug_phase,
	output wire        dac_valid,
	output wire [11:0] dac_data_i0,
	output wire [11:0] dac_data_q0,
	output wire [11:0] dac_data_i1,
	output wire [11:0] dac_data_q1
);

localparam [3:0] TX_SOQPSK_TG = 4'd3;
localparam [3:0] TX_PCM_FM = 4'd4;
localparam [3:0] TX_CW = 4'd6;
// PCM/FM and SOQPSK-TG share a 61.44 MSPS sample clock and the Q0.32
// scheduler.  Keep at least four complex samples per input BIT so the
// SOQPSK TG pulse and PCM Bi-phase half-BIT transitions remain represented.
localparam [31:0] STANDARD_MIN_RATE_WORD = 32'h000A_AAAB; // 10 kbit/s
localparam [31:0] STANDARD_MAX_RATE_WORD = 32'h5355_5555; // 20 Mbit/s at 61.44 MSPS

wire valid_tx1;
wire valid_tx2;
wire [11:0] legacy_i0;
wire [11:0] legacy_q0;
wire [11:0] legacy_i1;
wire [11:0] legacy_q1;
wire [11:0] cw_i0;
wire [11:0] cw_q0;
wire [11:0] cw_i1;
wire [11:0] cw_q1;
wire [11:0] pcm_i;
wire [11:0] pcm_q;
wire [11:0] soqpsk_i;
wire [11:0] soqpsk_q;
wire pcm_ready;
wire soqpsk_ready;
wire modulator_ready;
wire randomized_bit_data;
wire randomized_bit_valid;
wire pcm_iq_valid_int;
wire soqpsk_iq_valid;
wire [31:0] pcm_accepted_bits_int;
wire [31:0] soqpsk_accepted_bits;
wire signed [15:0] pcm_debug_nrz_int;
wire signed [15:0] pcm_debug_filter_int;
wire [31:0] pcm_debug_phase_int;
wire signed [15:0] soqpsk_debug_alpha;
wire signed [15:0] soqpsk_debug_frequency;
wire [31:0] soqpsk_debug_phase;

(* ASYNC_REG = "TRUE" *) reg [31:0] phase_meta, phase_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] deviation_meta, deviation_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] offset_meta, offset_sync;
(* ASYNC_REG = "TRUE" *) reg [15:0] amplitude_meta, amplitude_sync;
(* ASYNC_REG = "TRUE" *) reg [3:0] profile_meta, profile_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] control_meta, control_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] soqpsk_rate_meta, soqpsk_rate_sync;
reg control_clear_last;

wire [3:0] tx1_mode;
wire [3:0] tx2_mode;
wire pcm_selected;
wire soqpsk_selected;
wire mixed_selected;
wire pcm_enable;
wire soqpsk_enable;
wire standard_rate_valid;
wire common_parameters_valid;
wire pcm_deviation_valid;
// Compare the rational h=0.7 relation without a hardware divider.
// abs(100*dev - 35*rate) <= 50 admits nearest-integer frequency words.
wire [39:0] deviation_scaled = {8'd0,deviation_sync} * 40'd100;
wire [39:0] rate_scaled = {8'd0,soqpsk_rate_sync} * 40'd35;
wire [39:0] deviation_error = (deviation_scaled >= rate_scaled) ?
    deviation_scaled - rate_scaled : rate_scaled - deviation_scaled;
wire pcm_clear;
wire [2:0] line_code;
wire [2:0] rolloff;

assign tx1_mode = phase_sync[3:0];
assign tx2_mode = phase_sync[11:8];
assign pcm_selected = (tx1_mode == TX_PCM_FM) || (tx2_mode == TX_PCM_FM);
assign soqpsk_selected = (tx1_mode == TX_SOQPSK_TG) || (tx2_mode == TX_SOQPSK_TG);
assign mixed_selected = pcm_selected && soqpsk_selected;
assign standard_rate_valid = (soqpsk_rate_sync >= STANDARD_MIN_RATE_WORD) &&
                             (soqpsk_rate_sync <= STANDARD_MAX_RATE_WORD);
assign pcm_enable = pcm_selected && !mixed_selected && control_sync[0] &&
                    standard_rate_valid && common_parameters_valid &&
                    pcm_deviation_valid && (line_code <= 3'd6) &&
                    (control_sync[3] == (line_code == 3'd6));
// Match the PS packet contract: RNRZ-L enables IRIG15, the other six
// diagnostic/compatibility line codes bypass it. Reject reserved code 7
// rather than silently mapping it to NRZ-L. This is not an RF certification.
assign soqpsk_enable = soqpsk_selected && !mixed_selected && control_sync[0] &&
                       standard_rate_valid && common_parameters_valid &&
                       control_sync[2] && control_sync[3];
assign common_parameters_valid = !amplitude_sync[15] &&
                                 (offset_sync == 32'd0) && (profile_sync == 4'd0);
assign pcm_deviation_valid = deviation_error <= 40'd50;
// This product's standard SOQPSK path requires differential coding and IRIG15.
// Fail closed on stale/direct register writes, as the PS/UI already do.
assign pcm_clear = control_sync[1] && !control_clear_last;
// Shared shape controls are carried in the previously reserved PCM_CONTROL
// bits.  They are synchronized with the enable bit and therefore change only
// at a software transaction boundary.
assign line_code = control_sync[6:4];
assign rolloff = control_sync[9:7];

assign modulator_ready = mixed_selected ? 1'b0 :
                   (pcm_selected ? pcm_ready :
                   (soqpsk_selected ? soqpsk_ready : 1'b0));
assign bit_ready = modulator_ready;

irig_randomizer irig_randomizer_source (
	.clk(clk), .rst(rst), .clear(pcm_clear), .enable(control_sync[3]),
	.in_data(bit_data), .in_valid(bit_valid), .out_ready(modulator_ready),
	.out_data(randomized_bit_data), .out_valid(randomized_bit_valid)
);

always @(posedge clk or posedge rst) begin
	if (rst) begin
		phase_meta <= 32'd0; phase_sync <= 32'd0;
		deviation_meta <= 32'd0; deviation_sync <= 32'd0;
		offset_meta <= 32'd0; offset_sync <= 32'd0;
		amplitude_meta <= 16'd0; amplitude_sync <= 16'd0;
		profile_meta <= 4'd0; profile_sync <= 4'd0;
		control_meta <= 32'd0; control_sync <= 32'd0;
		soqpsk_rate_meta <= 32'd0; soqpsk_rate_sync <= 32'd0;
		control_clear_last <= 1'b0;
	end else begin
		phase_meta <= phase_inc; phase_sync <= phase_meta;
		deviation_meta <= pcm_deviation_word; deviation_sync <= deviation_meta;
		offset_meta <= pcm_frequency_offset; offset_sync <= offset_meta;
		amplitude_meta <= pcm_amplitude; amplitude_sync <= amplitude_meta;
		profile_meta <= pcm_filter_profile; profile_sync <= profile_meta;
		control_meta <= pcm_control; control_sync <= control_meta;
		soqpsk_rate_meta <= soqpsk_bit_rate_word; soqpsk_rate_sync <= soqpsk_rate_meta;
		control_clear_last <= control_sync[1];
	end
end

tx_waveform_channel tx1_source (
	.clk(clk), .rst(rst), .mode(tx1_mode), .rolloff(rolloff), .dac_valid(valid_tx1),
	.dac_data_i0(legacy_i0), .dac_data_q0(legacy_q0),
	.cw_data_i(cw_i0), .cw_data_q(cw_q0)
);

tx_waveform_channel tx2_source (
	.clk(clk), .rst(rst), .mode(tx2_mode), .rolloff(rolloff), .dac_valid(valid_tx2),
	.dac_data_i0(legacy_i1), .dac_data_q0(legacy_q1),
	.cw_data_i(cw_i1), .cw_data_q(cw_q1)
);

pcm_fm_modulator pcm_fm_source (
	.clk(clk), .rst(rst), .enable(pcm_enable), .clear(pcm_clear),
	.bit_data(randomized_bit_data), .bit_valid(randomized_bit_valid), .bit_ready(pcm_ready),
	.bit_phase(bit_phase),
	.bit_rate_word(soqpsk_rate_sync),
	.deviation_word(deviation_sync),
	.frequency_offset_word(offset_sync),
	.amplitude(amplitude_sync), .filter_profile(profile_sync),
	.line_code(line_code), .rolloff(rolloff),
	.data_i(pcm_i), .data_q(pcm_q), .iq_valid(pcm_iq_valid_int),
	.accepted_bits(pcm_accepted_bits_int), .debug_nrz(pcm_debug_nrz_int),
	.debug_filter(pcm_debug_filter_int), .debug_phase(pcm_debug_phase_int)
);

soqpsk_tg_modulator soqpsk_tg_source (
	.clk(clk), .rst(rst), .enable(soqpsk_enable), .clear(pcm_clear),
	.differential_enable(control_sync[2]),
	.bit_data(randomized_bit_data), .bit_valid(randomized_bit_valid), .bit_ready(soqpsk_ready),
	.bit_phase(bit_phase),
	.bit_rate_word(soqpsk_rate_sync),
	.frequency_offset_word(offset_sync), .amplitude(amplitude_sync),
	.data_i(soqpsk_i), .data_q(soqpsk_q), .iq_valid(soqpsk_iq_valid),
	.accepted_bits(soqpsk_accepted_bits), .debug_alpha(soqpsk_debug_alpha),
	.debug_frequency(soqpsk_debug_frequency), .debug_phase(soqpsk_debug_phase)
);

assign pcm_iq_valid = soqpsk_selected ? soqpsk_iq_valid : pcm_iq_valid_int;
assign pcm_accepted_bits = soqpsk_selected ? soqpsk_accepted_bits : pcm_accepted_bits_int;
assign pcm_debug_nrz = soqpsk_selected ? soqpsk_debug_alpha : pcm_debug_nrz_int;
assign pcm_debug_filter = soqpsk_selected ? soqpsk_debug_frequency : pcm_debug_filter_int;
assign pcm_debug_phase = soqpsk_selected ? soqpsk_debug_phase : pcm_debug_phase_int;

tx_waveform_mux tx1_mux (
	.mode(tx1_mode), .legacy_i(legacy_i0), .legacy_q(legacy_q0),
	.cw_i(cw_i0), .cw_q(cw_q0),
	.soqpsk_i(soqpsk_i), .soqpsk_q(soqpsk_q),
	.pcm_i(pcm_i), .pcm_q(pcm_q),
	.selected_i(dac_data_i0), .selected_q(dac_data_q0)
);

tx_waveform_mux tx2_mux (
	.mode(tx2_mode), .legacy_i(legacy_i1), .legacy_q(legacy_q1),
	.cw_i(cw_i1), .cw_q(cw_q1),
	.soqpsk_i(soqpsk_i), .soqpsk_q(soqpsk_q),
	.pcm_i(pcm_i), .pcm_q(pcm_q),
	.selected_i(dac_data_i1), .selected_q(dac_data_q1)
);

assign dac_valid = valid_tx1 & valid_tx2;

wire unused_pcm_status = &{1'b0, frame_start, frame_end, artm_bit_rate_word[0]};

endmodule
