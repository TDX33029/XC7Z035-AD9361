`timescale 1ns / 1ps

/* IRIG-106 self-synchronizing randomizer: h(x)=x^15+x^14+1. */
module irig_randomizer (
	input wire clk, input wire rst, input wire clear, input wire enable,
	input wire in_data, input wire in_valid, input wire out_ready,
	output wire out_data, output wire out_valid
);
localparam [14:0] INITIAL_STATE = 15'b000000000000001;
reg [14:0] state;
wire feedback = state[13] ^ state[14];
assign out_data = enable ? (in_data ^ feedback) : in_data;
assign out_valid = in_valid;
always @(posedge clk or posedge rst) begin
	if (rst) state <= INITIAL_STATE;
	else if (clear || !enable) state <= INITIAL_STATE;
	else if (in_valid && out_ready) state <= {state[13:0], out_data};
end
endmodule
