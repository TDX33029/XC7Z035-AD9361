`timescale 1ns/1ps
// 独立于ARM指令执行的FCLK100域联锁；不依赖PS中断或软件停发函数。
// 不是独立晶振/整板掉电保护。GPIO8心跳，9 ARM，10要求TFS LD。
module p351_rf_safety #(
    parameter integer TIMEOUT_CYCLES=25000000
)(
    input wire clk, resetn,
    input wire heartbeat, arm, rf_request, require_tfs, tfs_ld, reference_ok,
    output wire ready, fault, force_atten_safe,
    output wire rf_allow
);
    (* ASYNC_REG="TRUE" *) reg [1:0] hb_sync, arm_sync, req_sync, need_sync, ld_sync;
    reg hb_last, latched;
    localparam integer CW=$clog2(TIMEOUT_CYCLES+1);
    reg [CW-1:0] age;
    assign fault=latched;
    assign ready=resetn && arm_sync[1] && !latched;
    assign rf_allow=ready && reference_ok && (!need_sync[1] || ld_sync[1]);
    assign force_atten_safe=!resetn || latched;
    always @(posedge clk) begin
        if(!resetn) begin
            hb_sync<=0; arm_sync<=0; req_sync<=0; need_sync<=0; ld_sync<=0;
            hb_last<=0; age<=0; latched<=0;
        end else begin
            hb_sync<={hb_sync[0],heartbeat}; arm_sync<={arm_sync[0],arm};
            req_sync<={req_sync[0],rf_request}; need_sync<={need_sync[0],require_tfs};
            ld_sync<={ld_sync[0],tfs_ld}; hb_last<=hb_sync[1];
            // 只有已撤销RF请求且显式解除ARM，才允许清故障；LD/心跳恢复不自启。
            if(!arm_sync[1] && !req_sync[1]) begin latched<=0; age<=0; end
            else if(arm_sync[1] && req_sync[1]) begin
                if(!reference_ok || (need_sync[1] && !ld_sync[1])) latched<=1;
                if(hb_sync[1]!=hb_last) age<=0;
                else if(age<TIMEOUT_CYCLES-1) age<=age+1'b1;
                else latched<=1;
            end else age<=0;
        end
    end
endmodule
