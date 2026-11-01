`timescale 1ns / 1ps

module lvds_prbs_checker_core (
    input  wire        data_clk,
    input  wire        rst,
    input  wire        enable,
    input  wire        clear,
    input  wire [31:0] window_samples,
    input  wire        snapshot_req,
    output reg         snapshot_ack,
    input  wire        adc_valid,
    input  wire [11:0] adc_data_i0,
    input  wire [11:0] adc_data_q0,
    input  wire [11:0] adc_data_i1,
    input  wire [11:0] adc_data_q1,
    output reg  [31:0] status_snapshot,
    output reg  [31:0] sample_snapshot,
    output reg  [31:0] error_i0_snapshot,
    output reg  [31:0] error_q0_snapshot,
    output reg  [31:0] error_i1_snapshot,
    output reg  [31:0] error_q1_snapshot,
    output reg  [31:0] gap_error_snapshot,
    output reg  [31:0] oos_snapshot,
    output reg  [31:0] raw0_snapshot,
    output reg  [31:0] raw1_snapshot,
    output reg  [31:0] raw2_snapshot,
    output reg  [31:0] raw3_snapshot,
    output reg  [31:0] raw4_snapshot
);
    reg enable_d;
    wire start = enable && !enable_d;
    reg busy, done;
    reg [31:0] sample_count;
    reg [31:0] last_sample;
    reg [31:0] err_i0, err_q0, err_i1, err_q1;
    reg [31:0] gap_error, oos_count;
    reg [31:0] raw0, raw1, raw2, raw3, raw4;
    reg [2:0] gap_count;
    reg seen_valid;
    reg checker_valid;
    reg all_locked_d;
    reg [11:0] sample_i0, sample_q0, sample_i1, sample_q1;
    wire checker_clear = clear || start;
    // A valid 1R1T sample is expected once every two DATA_CLK cycles.  Reject
    // early pulses so the PN state has the two-cycle interval in the XDC.
    wire accept_valid = adc_valid && (!seen_valid || gap_count >= 3'd1);

    wire l_i0, l_q0, l_i1, l_q1;
    wire [1:0] mode_i0, mode_q0, mode_i1, mode_q1;
    wire [5:0] e_i0, e_q0, e_i1, e_q1;
    wire o_i0, o_q0, o_i1, o_q1;

    // Register the PHY sample at the checker boundary.  Besides isolating the
    // RX decoder fanout, this makes every expensive PN9 path originate from a
    // register that is guaranteed to remain unchanged for two DATA_CLK cycles.
    pn9_word_checker #(.Q_OR_I_N(0)) c_i0 (.clk(data_clk),.rst(rst),.clear(checker_clear),.valid(checker_valid),.data_i(sample_i0),.data_q(sample_q0),.locked(l_i0),.selected_mode(mode_i0),.error_bits(e_i0),.oos_pulse(o_i0));
    pn9_word_checker #(.Q_OR_I_N(1)) c_q0 (.clk(data_clk),.rst(rst),.clear(checker_clear),.valid(checker_valid),.data_i(sample_i0),.data_q(sample_q0),.locked(l_q0),.selected_mode(mode_q0),.error_bits(e_q0),.oos_pulse(o_q0));
    assign l_i1 = 1'b0, l_q1 = 1'b0;
    assign mode_i1 = 2'b00, mode_q1 = 2'b00;
    assign e_i1 = 6'd0, e_q1 = 6'd0;
    assign o_i1 = 1'b0, o_q1 = 1'b0;

    wire [3:0] locks = {l_q1,l_i1,l_q0,l_i0};
    wire any_error = (err_i0|err_q0|err_i1|err_q1|gap_error|oos_count) != 0;
    wire [7:0] modes = {mode_q1,mode_i1,mode_q0,mode_i0};

    always @(posedge data_clk or posedge rst) begin
        if (rst) begin
            enable_d <= 0; busy <= 0; done <= 0;
            sample_count <= 0; last_sample <= 0;
            err_i0 <= 0; err_q0 <= 0; err_i1 <= 0; err_q1 <= 0;
            gap_error <= 0; oos_count <= 0; gap_count <= 0; seen_valid <= 0;
            raw0 <= 0; raw1 <= 0; raw2 <= 0; raw3 <= 0; raw4 <= 0;
            checker_valid <= 0;
            all_locked_d <= 0;
            sample_i0 <= 0; sample_q0 <= 0; sample_i1 <= 0; sample_q1 <= 0;
            snapshot_ack <= 0;
            status_snapshot <= 0; sample_snapshot <= 0;
            error_i0_snapshot <= 0; error_q0_snapshot <= 0;
            error_i1_snapshot <= 0; error_q1_snapshot <= 0;
            gap_error_snapshot <= 0; oos_snapshot <= 0;
            raw0_snapshot <= 0; raw1_snapshot <= 0; raw2_snapshot <= 0;
            raw3_snapshot <= 0; raw4_snapshot <= 0;
        end else begin
            enable_d <= enable;
            checker_valid <= 0;
            all_locked_d <= &locks[1:0];
            if (!enable) busy <= 0;
            if (checker_clear) begin
                busy <= enable;
                done <= 0; sample_count <= 0;
                last_sample <= (window_samples == 0) ? 0 : window_samples - 1'b1;
                err_i0 <= 0; err_q0 <= 0; err_i1 <= 0; err_q1 <= 0;
                gap_error <= 0; oos_count <= 0; gap_count <= 0; seen_valid <= 0;
                raw0 <= 0; raw1 <= 0; raw2 <= 0; raw3 <= 0; raw4 <= 0;
            end else if (busy) begin
                if (gap_count != 3'd7) gap_count <= gap_count + 1'b1;
                if (adc_valid) begin
                    if (seen_valid && gap_count != 3'd1) gap_error <= gap_error + 1'b1;
                    gap_count <= 0;
                    seen_valid <= 1'b1;
                    if (accept_valid) begin
                        sample_i0 <= adc_data_i0; sample_q0 <= adc_data_q0;
                        sample_i1 <= adc_data_i1; sample_q1 <= adc_data_q1;
                        checker_valid <= 1;
                    end
                end
            end

            // checker_valid is the one-cycle-delayed accepted-sample strobe.
            // It can remain valid for the final word after busy is deasserted.
            if ((&locks[1:0]) && !all_locked_d) begin
                // Discard acquisition mismatches; subsequent counters are BER
                // measurements for the selected, locked PRBS format only.
                err_i0 <= 0; err_q0 <= 0; err_i1 <= 0; err_q1 <= 0;
                oos_count <= 0;
            end else if (checker_valid && !checker_clear) begin
                case (sample_count)
                    32'd0: begin
                        raw0 <= {8'd0, sample_i0, sample_q0};
                    end
                    32'd1: raw1 <= {8'd0, sample_i0, sample_q0};
                    32'd2: raw2 <= {8'd0, sample_i0, sample_q0};
                    32'd3: raw3 <= {8'd0, sample_i0, sample_q0};
                    32'd4: raw4 <= {8'd0, sample_i0, sample_q0};
                    default: ;
                endcase
                sample_count <= sample_count + 1'b1;
                err_i0 <= err_i0 + e_i0; err_q0 <= err_q0 + e_q0;
                err_i1 <= err_i1 + e_i1; err_q1 <= err_q1 + e_q1;
                if (o_i0|o_q0|o_i1|o_q1) oos_count <= oos_count + 1'b1;
                if (sample_count == last_sample) begin
                    busy <= 0;
                    done <= 1;
                end
            end

            if (snapshot_req != snapshot_ack) begin
                status_snapshot <= {snapshot_req, 15'd0, modes,
                                    (gap_error!=0), any_error, locks, done, busy};
                sample_snapshot <= sample_count;
                error_i0_snapshot <= err_i0; error_q0_snapshot <= err_q0;
                error_i1_snapshot <= err_i1; error_q1_snapshot <= err_q1;
                gap_error_snapshot <= gap_error; oos_snapshot <= oos_count;
                raw0_snapshot <= raw0; raw1_snapshot <= raw1;
                raw2_snapshot <= raw2; raw3_snapshot <= raw3;
                raw4_snapshot <= raw4;
                snapshot_ack <= snapshot_req;
            end
        end
    end
endmodule
