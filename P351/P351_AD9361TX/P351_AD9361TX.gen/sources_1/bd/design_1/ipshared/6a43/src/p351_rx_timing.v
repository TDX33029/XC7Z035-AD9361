`timescale 1ns/1ps
// 已知名义码率的连续位同步：PCM鉴频用Gardner，TG逆均衡输出用三电平M&M。
// 不读PN/同步字/发送端数据。频率积分保留Q40小数，避免低码率量化偏置。
module p351_rx_timing #(
    parameter integer TRACKING_ENABLE=1
)(input clk,input reset,input soq_mode,input biphase,input [1:0] symbol_phase,
  input [31:0] nominal_rate,input [23:0] nominal_recip,
  input sample_valid,input signed [17:0] sample,
  input soq_valid,input signed [17:0] soq_sample,
  output reg [31:0] adjusted_rate=0,output reg [23:0] adjusted_recip=0,
  output wire signed [31:0] trim_q24);
    reg [1:0] quarter=0;
    reg signed [17:0] history0=0,history1=0,previous=0;
    reg signed [1:0] previous_decision=0;
    reg [7:0] warm=0;
    reg [3:0] vp=0;
    reg signed [18:0] difference=0;
    reg signed [17:0] midpoint=0;
    reg signed [19:0] mm_error=0;
    reg signed [39:0] error_q30=0;
    reg signed [41:0] integral_q40=0;
    reg signed [21:0] control_q24=0;
    reg signed [54:0] rate_product=0;
    reg signed [46:0] reciprocal_product=0;
    // 单独做有符号移位，避免与unsigned名义值相加时被转换成逻辑右移。
    wire signed [54:0] rate_delta=rate_product>>>24;
    wire signed [46:0] reciprocal_delta=reciprocal_product>>>24;
    wire signed [41:0] next_integral=integral_q40-(error_q30>>>6);
    wire signed [41:0] limited_integral=next_integral>42'sd1099511628 ? 42'sd1099511628 :
        next_integral< -42'sd1099511628 ? -42'sd1099511628 : next_integral;
    wire signed [41:0] control_value=(limited_integral>>>16)-(error_q30>>>13);
    wire signed [18:0] pcm_difference=$signed(previous)-$signed(sample);
    wire signed [36:0] pcm_error=$signed(difference)*$signed(midpoint);
    function signed [1:0] decide;
        input signed [17:0] x;
        begin decide=x>18'sd16384 ? 2'sd1 : x< -18'sd16384 ? -2'sd1 : 2'sd0;end
    endfunction
    wire signed [1:0] this_decision=decide(soq_sample);
    wire signed [19:0] mm_left=previous_decision==1?$signed(soq_sample):
        previous_decision==-1?-$signed({soq_sample[17],soq_sample}):20'sd0;
    wire signed [19:0] mm_right=this_decision==1?$signed(previous):
        this_decision==-1?-$signed({previous[17],previous}):20'sd0;
    assign trim_q24=TRACKING_ENABLE?$signed(integral_q40>>>16):32'sd0;
    always @(posedge clk) begin
        if(reset) begin
            quarter<=0;history0<=0;history1<=0;previous<=0;previous_decision<=0;warm<=0;
            vp<=0;difference<=0;midpoint<=0;mm_error<=0;error_q30<=0;
            integral_q40<=0;control_q24<=0;rate_product<=0;reciprocal_product<=0;
            adjusted_rate<=nominal_rate;adjusted_recip<=nominal_recip;
        end else begin
            vp<={vp[2:0],1'b0};
            if(sample_valid && !soq_mode) begin
                history0<=sample;history1<=history0;quarter<=quarter+1'b1;
                if(biphase ? quarter[0]==symbol_phase[0] : quarter==symbol_phase) begin
                    // 双相码每半位一次Gardner更新，2sps半位；码率字仍为bit/s。
                    difference<=pcm_difference;midpoint<=biphase?history0:history1;previous<=sample;
                    if(warm<255)warm<=warm+1'b1;
                    if(warm>=64 && TRACKING_ENABLE)vp[0]<=1;
                end
            end
            if(soq_valid && soq_mode) begin
                mm_error<=mm_left-mm_right;previous<=soq_sample;previous_decision<=this_decision;
                if(warm<255)warm<=warm+1'b1;
                if(warm>=64 && TRACKING_ENABLE)vp[0]<=1;
            end
            if(vp[0]) begin
                // PCM/FM频偏0.35Rb：幅度约0.35*32768，以8倍能量归一化。
                if(soq_mode)error_q30<=$signed(mm_error)<<<15;
                else error_q30<=$signed(pcm_error)<<<3;
            end
            if(vp[1]) begin
                integral_q40<=limited_integral; // ±1000ppm，不能自动改变名义配置。
                control_q24<=control_value>42'sd524288 ? 22'sd524288 :
                    control_value< -42'sd524288 ? -22'sd524288 : control_value[21:0];
            end
            if(vp[2]) begin
                rate_product<=$signed({1'b0,nominal_rate})*$signed(control_q24);
                reciprocal_product<=$signed({1'b0,nominal_recip})*$signed(control_q24);
            end
            if(vp[3]) begin
                adjusted_rate<=nominal_rate+rate_delta[31:0];
                // 小范围1/(1+e)≈1-e；瞬态|e|≤1/32，正值/采样单调性保持。
                adjusted_recip<=nominal_recip-reciprocal_delta[23:0];
            end
        end
    end
endmodule
