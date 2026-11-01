`timescale 1ns/1ps
// 100MHz域独立测量所选10MHz支路，不依靠可能陈旧的ADF4002 DLD。
// 1ms窗口应有10000次上升沿。±2%仅为存在/粗频率资格，不是ppm精度测量。
module p351_ref_monitor #(
    parameter integer WINDOW_CYCLES = 100000,
    parameter integer MIN_EDGES = 9800,
    parameter integer MAX_EDGES = 10200,
    parameter integer NO_EDGE_CYCLES = 50
)(
    input wire clk, resetn,
    input wire ref_in, pll_lock, require_external, arm_external,
    output wire ref_present, frequency_ok, qualified
);
    (* ASYNC_REG="TRUE" *) reg [1:0] ref_sync, lock_sync, req_sync, arm_sync;
    reg ref_previous, req_previous;
    reg [16:0] window_count, edge_count;
    reg [7:0] idle_count;
    reg frequency_reg;
    reg fault_latched;
    wire edge_rise = ref_sync[1] & ~ref_previous;
    wire request_rise = req_sync[1] & ~req_previous;
    wire [17:0] edges_final = {1'b0,edge_count} + edge_rise;
    assign ref_present = resetn && idle_count < NO_EDGE_CYCLES;
    assign frequency_ok = resetn && frequency_reg;
    wire raw_qualified = ref_present && frequency_ok && lock_sync[1];
    assign qualified = raw_qualified && !fault_latched;
    always @(posedge clk) begin
        if (!resetn) begin
            ref_sync<=0;lock_sync<=0;req_sync<=0;arm_sync<=0;
            ref_previous<=0;req_previous<=0;
            window_count<=0;edge_count<=0;idle_count<=NO_EDGE_CYCLES;frequency_reg<=0;fault_latched<=0;
        end else begin
            ref_sync <= {ref_sync[0],ref_in};
            lock_sync <= {lock_sync[0],pll_lock};
            req_sync <= {req_sync[0],require_external};
            arm_sync <= {arm_sync[0],arm_external};
            ref_previous <= ref_sync[1];req_previous<=req_sync[1];
            // 仅在PS宣布稳定锁定后锁存故障，允许初次捕获时DLD正常抖动。
            // 不因信号恢复或软件取消ARM清故障；必须切REQ或复位。
            if (!req_sync[1] || request_rise) fault_latched<=0;
            else if (arm_sync[1] && !raw_qualified) fault_latched<=1;
            if (edge_rise) idle_count<=0;
            else if (idle_count < NO_EDGE_CYCLES) idle_count<=idle_count+1'b1;
            if (request_rise) begin
                window_count<=0;edge_count<=0;frequency_reg<=0;
            end else if (window_count == WINDOW_CYCLES-1) begin
                window_count<=0;edge_count<=0;
                frequency_reg <= edges_final >= MIN_EDGES && edges_final <= MAX_EDGES;
            end else begin
                window_count<=window_count+1'b1;
                if (edge_rise) edge_count<=edge_count+1'b1;
            end
        end
    end
endmodule
