`timescale 1ns/1ps
// P351接收DSP原型：真实RX I/Q鉴频 -> 可配置4倍码率采样 -> 判决。
// SOQPSK使用固定有限长TG均衡/四状态检测；不是最优相干CPM MLSE。
// 参数只在enable=0时由上层提交。所有处理使用真实输入，不生成替代PN数据。
module p351_rx_discriminator(input clk,input reset,input valid,
    input signed [11:0] i_data,q_data,
    output out_valid,output signed [31:0] angle_word);
    reg seen=0;reg signed [11:0] pi,pq;
    reg signed [23:0] ii,qq,qi,iq;
    reg v0=0,v1=0;
    reg signed [24:0] cross_x,cross_y;
    function [31:0] atan_word(input integer k);
        begin case(k)
        0:atan_word=32'h20000000;
        1:atan_word=32'h12e4051e;
        2:atan_word=32'h09fb385b;
        3:atan_word=32'h051111d4;
        4:atan_word=32'h028b0d43;
        5:atan_word=32'h0145d7e1;
        6:atan_word=32'h00a2f61e;
        7:atan_word=32'h00517c55;
        8:atan_word=32'h0028be53;
        9:atan_word=32'h00145f2f;
        10:atan_word=32'h000a2f98;
        11:atan_word=32'h000517cc;
        12:atan_word=32'h00028be6;
        13:atan_word=32'h000145f3;
        14:atan_word=32'h0000a2fa;
        15:atan_word=32'h0000517d;
        16:atan_word=32'h000028be;
        17:atan_word=32'h0000145f;
        18:atan_word=32'h00000a30;
        19:atan_word=32'h00000518;
        default:atan_word=0;endcase end
    endfunction
    reg signed [34:0] x[0:20],y[0:20];
    reg signed [31:0] z[0:20];reg [20:0] vp=0;
    integer k;
    always @(posedge clk) begin
        if(reset) begin seen<=0;v0<=0;v1<=0;vp<=0;end
        else begin
            v0<=valid && seen;v1<=v0;
            if(valid) begin
                pi<=i_data;pq<=q_data;seen<=1;
                ii<=i_data*pi;qq<=q_data*pq;qi<=q_data*pi;iq<=i_data*pq;
            end
            if(v0) begin
                cross_x<=$signed({ii[23],ii})+$signed({qq[23],qq});
                cross_y<=$signed({qi[23],qi})-$signed({iq[23],iq});
            end
            vp[0]<=v1;
            if(v1) begin
                if(cross_x<0) begin
                    x[0]<=-($signed(cross_x)<<<8);y[0]<=-($signed(cross_y)<<<8);
                    z[0]<=32'h80000000;
                end else begin x[0]<=$signed(cross_x)<<<8;y[0]<=$signed(cross_y)<<<8;z[0]<=0;end
            end
            for(k=0;k<20;k=k+1) begin
                vp[k+1]<=vp[k];
                if(y[k]>=0) begin
                    x[k+1]<=x[k]+(y[k]>>>k);y[k+1]<=y[k]-(x[k]>>>k);z[k+1]<=z[k]+$signed(atan_word(k));
                end else begin
                    x[k+1]<=x[k]-(y[k]>>>k);y[k+1]<=y[k]+(x[k]>>>k);z[k+1]<=z[k]-$signed(atan_word(k));
                end
            end
        end
    end
    assign out_valid=vp[20];assign angle_word=z[20];
endmodule

module p351_soq_fir(input clk,input reset,input valid,input signed [17:0] sample,
    output out_valid,output signed [17:0] out_sample);
    // 65个三元符号的有限支撑最小二乘逆，292个4sps抽头，Q12。
    // 系数由streaming_equalizer_probe.coefficients独立生成并核验。
    function signed [17:0] coefficient(input integer index);
        begin case(index)
        0:coefficient=18'sd0;
        1:coefficient=18'sd0;
        2:coefficient=18'sd0;
        3:coefficient=18'sd0;
        4:coefficient=18'sd0;
        5:coefficient=-18'sd2;
        6:coefficient=-18'sd6;
        7:coefficient=-18'sd11;
        8:coefficient=-18'sd13;
        9:coefficient=-18'sd6;
        10:coefficient=18'sd13;
        11:coefficient=18'sd46;
        12:coefficient=18'sd81;
        13:coefficient=18'sd104;
        14:coefficient=18'sd100;
        15:coefficient=18'sd61;
        16:coefficient=-18'sd4;
        17:coefficient=-18'sd69;
        18:coefficient=-18'sd107;
        19:coefficient=-18'sd93;
        20:coefficient=-18'sd32;
        21:coefficient=18'sd50;
        22:coefficient=18'sd115;
        23:coefficient=18'sd122;
        24:coefficient=18'sd64;
        25:coefficient=-18'sd31;
        26:coefficient=-18'sd118;
        27:coefficient=-18'sd144;
        28:coefficient=-18'sd91;
        29:coefficient=18'sd16;
        30:coefficient=18'sd124;
        31:coefficient=18'sd167;
        32:coefficient=18'sd117;
        33:coefficient=-18'sd1;
        34:coefficient=-18'sd131;
        35:coefficient=-18'sd191;
        36:coefficient=-18'sd145;
        37:coefficient=-18'sd12;
        38:coefficient=18'sd141;
        39:coefficient=18'sd220;
        40:coefficient=18'sd175;
        41:coefficient=18'sd26;
        42:coefficient=-18'sd155;
        43:coefficient=-18'sd253;
        44:coefficient=-18'sd210;
        45:coefficient=-18'sd40;
        46:coefficient=18'sd172;
        47:coefficient=18'sd293;
        48:coefficient=18'sd249;
        49:coefficient=18'sd55;
        50:coefficient=-18'sd194;
        51:coefficient=-18'sd341;
        52:coefficient=-18'sd295;
        53:coefficient=-18'sd71;
        54:coefficient=18'sd221;
        55:coefficient=18'sd397;
        56:coefficient=18'sd349;
        57:coefficient=18'sd89;
        58:coefficient=-18'sd253;
        59:coefficient=-18'sd463;
        60:coefficient=-18'sd412;
        61:coefficient=-18'sd110;
        62:coefficient=18'sd293;
        63:coefficient=18'sd541;
        64:coefficient=18'sd485;
        65:coefficient=18'sd133;
        66:coefficient=-18'sd339;
        67:coefficient=-18'sd634;
        68:coefficient=-18'sd571;
        69:coefficient=-18'sd160;
        70:coefficient=18'sd395;
        71:coefficient=18'sd743;
        72:coefficient=18'sd672;
        73:coefficient=18'sd191;
        74:coefficient=-18'sd461;
        75:coefficient=-18'sd871;
        76:coefficient=-18'sd790;
        77:coefficient=-18'sd227;
        78:coefficient=18'sd538;
        79:coefficient=18'sd1023;
        80:coefficient=18'sd930;
        81:coefficient=18'sd269;
        82:coefficient=-18'sd630;
        83:coefficient=-18'sd1200;
        84:coefficient=-18'sd1093;
        85:coefficient=-18'sd319;
        86:coefficient=18'sd738;
        87:coefficient=18'sd1409;
        88:coefficient=18'sd1286;
        89:coefficient=18'sd377;
        90:coefficient=-18'sd864;
        91:coefficient=-18'sd1655;
        92:coefficient=-18'sd1512;
        93:coefficient=-18'sd446;
        94:coefficient=18'sd1012;
        95:coefficient=18'sd1943;
        96:coefficient=18'sd1778;
        97:coefficient=18'sd529;
        98:coefficient=-18'sd1184;
        99:coefficient=-18'sd2280;
        100:coefficient=-18'sd2092;
        101:coefficient=-18'sd628;
        102:coefficient=18'sd1383;
        103:coefficient=18'sd2676;
        104:coefficient=18'sd2463;
        105:coefficient=18'sd750;
        106:coefficient=-18'sd1612;
        107:coefficient=-18'sd3138;
        108:coefficient=-18'sd2901;
        109:coefficient=-18'sd901;
        110:coefficient=18'sd1870;
        111:coefficient=18'sd3676;
        112:coefficient=18'sd3422;
        113:coefficient=18'sd1095;
        114:coefficient=-18'sd2154;
        115:coefficient=-18'sd4299;
        116:coefficient=-18'sd4044;
        117:coefficient=-18'sd1351;
        118:coefficient=18'sd2455;
        119:coefficient=18'sd5014;
        120:coefficient=18'sd4794;
        121:coefficient=18'sd1704;
        122:coefficient=-18'sd2745;
        123:coefficient=-18'sd5824;
        124:coefficient=-18'sd5707;
        125:coefficient=-18'sd2212;
        126:coefficient=18'sd2974;
        127:coefficient=18'sd6723;
        128:coefficient=18'sd6843;
        129:coefficient=18'sd2981;
        130:coefficient=-18'sd3034;
        131:coefficient=-18'sd7667;
        132:coefficient=-18'sd8268;
        133:coefficient=-18'sd4173;
        134:coefficient=18'sd2752;
        135:coefficient=18'sd8620;
        136:coefficient=18'sd10183;
        137:coefficient=18'sd6210;
        138:coefficient=-18'sd1602;
        139:coefficient=-18'sd9102;
        140:coefficient=-18'sd12302;
        141:coefficient=-18'sd9052;
        142:coefficient=-18'sd452;
        143:coefficient=18'sd9446;
        144:coefficient=18'sd16038;
        145:coefficient=18'sd16038;
        146:coefficient=18'sd9446;
        147:coefficient=-18'sd452;
        148:coefficient=-18'sd9052;
        149:coefficient=-18'sd12302;
        150:coefficient=-18'sd9102;
        151:coefficient=-18'sd1602;
        152:coefficient=18'sd6210;
        153:coefficient=18'sd10183;
        154:coefficient=18'sd8620;
        155:coefficient=18'sd2752;
        156:coefficient=-18'sd4173;
        157:coefficient=-18'sd8268;
        158:coefficient=-18'sd7667;
        159:coefficient=-18'sd3034;
        160:coefficient=18'sd2981;
        161:coefficient=18'sd6843;
        162:coefficient=18'sd6723;
        163:coefficient=18'sd2974;
        164:coefficient=-18'sd2212;
        165:coefficient=-18'sd5707;
        166:coefficient=-18'sd5824;
        167:coefficient=-18'sd2745;
        168:coefficient=18'sd1704;
        169:coefficient=18'sd4794;
        170:coefficient=18'sd5014;
        171:coefficient=18'sd2455;
        172:coefficient=-18'sd1351;
        173:coefficient=-18'sd4044;
        174:coefficient=-18'sd4299;
        175:coefficient=-18'sd2154;
        176:coefficient=18'sd1095;
        177:coefficient=18'sd3422;
        178:coefficient=18'sd3676;
        179:coefficient=18'sd1870;
        180:coefficient=-18'sd901;
        181:coefficient=-18'sd2901;
        182:coefficient=-18'sd3138;
        183:coefficient=-18'sd1612;
        184:coefficient=18'sd750;
        185:coefficient=18'sd2463;
        186:coefficient=18'sd2676;
        187:coefficient=18'sd1383;
        188:coefficient=-18'sd628;
        189:coefficient=-18'sd2092;
        190:coefficient=-18'sd2280;
        191:coefficient=-18'sd1184;
        192:coefficient=18'sd529;
        193:coefficient=18'sd1778;
        194:coefficient=18'sd1943;
        195:coefficient=18'sd1012;
        196:coefficient=-18'sd446;
        197:coefficient=-18'sd1512;
        198:coefficient=-18'sd1655;
        199:coefficient=-18'sd864;
        200:coefficient=18'sd377;
        201:coefficient=18'sd1286;
        202:coefficient=18'sd1409;
        203:coefficient=18'sd738;
        204:coefficient=-18'sd319;
        205:coefficient=-18'sd1093;
        206:coefficient=-18'sd1200;
        207:coefficient=-18'sd630;
        208:coefficient=18'sd269;
        209:coefficient=18'sd930;
        210:coefficient=18'sd1023;
        211:coefficient=18'sd538;
        212:coefficient=-18'sd227;
        213:coefficient=-18'sd790;
        214:coefficient=-18'sd871;
        215:coefficient=-18'sd461;
        216:coefficient=18'sd191;
        217:coefficient=18'sd672;
        218:coefficient=18'sd743;
        219:coefficient=18'sd395;
        220:coefficient=-18'sd160;
        221:coefficient=-18'sd571;
        222:coefficient=-18'sd634;
        223:coefficient=-18'sd339;
        224:coefficient=18'sd133;
        225:coefficient=18'sd485;
        226:coefficient=18'sd541;
        227:coefficient=18'sd293;
        228:coefficient=-18'sd110;
        229:coefficient=-18'sd412;
        230:coefficient=-18'sd463;
        231:coefficient=-18'sd253;
        232:coefficient=18'sd89;
        233:coefficient=18'sd349;
        234:coefficient=18'sd397;
        235:coefficient=18'sd221;
        236:coefficient=-18'sd71;
        237:coefficient=-18'sd295;
        238:coefficient=-18'sd341;
        239:coefficient=-18'sd194;
        240:coefficient=18'sd55;
        241:coefficient=18'sd249;
        242:coefficient=18'sd293;
        243:coefficient=18'sd172;
        244:coefficient=-18'sd40;
        245:coefficient=-18'sd210;
        246:coefficient=-18'sd253;
        247:coefficient=-18'sd155;
        248:coefficient=18'sd26;
        249:coefficient=18'sd175;
        250:coefficient=18'sd220;
        251:coefficient=18'sd141;
        252:coefficient=-18'sd12;
        253:coefficient=-18'sd145;
        254:coefficient=-18'sd191;
        255:coefficient=-18'sd131;
        256:coefficient=-18'sd1;
        257:coefficient=18'sd117;
        258:coefficient=18'sd167;
        259:coefficient=18'sd124;
        260:coefficient=18'sd16;
        261:coefficient=-18'sd91;
        262:coefficient=-18'sd144;
        263:coefficient=-18'sd118;
        264:coefficient=-18'sd31;
        265:coefficient=18'sd64;
        266:coefficient=18'sd122;
        267:coefficient=18'sd115;
        268:coefficient=18'sd50;
        269:coefficient=-18'sd32;
        270:coefficient=-18'sd93;
        271:coefficient=-18'sd107;
        272:coefficient=-18'sd69;
        273:coefficient=-18'sd4;
        274:coefficient=18'sd61;
        275:coefficient=18'sd100;
        276:coefficient=18'sd104;
        277:coefficient=18'sd81;
        278:coefficient=18'sd46;
        279:coefficient=18'sd13;
        280:coefficient=-18'sd6;
        281:coefficient=-18'sd13;
        282:coefficient=-18'sd11;
        283:coefficient=-18'sd6;
        284:coefficient=-18'sd2;
        285:coefficient=18'sd0;
        286:coefficient=18'sd0;
        287:coefficient=18'sd0;
        288:coefficient=18'sd0;
        289:coefficient=18'sd0;
        290:coefficient=18'sd0;
        291:coefficient=18'sd0;
        default:coefficient=0;endcase end
    endfunction
    reg signed [17:0] history[0:290];reg [8:0] warm=0;reg [9:0] vp=0;
    wire signed [43:0] sums[0:1022];
    integer k;
    always @(posedge clk) begin
        if(reset) begin warm<=0;vp<=0;end
        else begin
            vp<={vp[8:0],valid && warm>=291};
            if(valid) begin
                if(warm<291) warm<=warm+1'b1;
                history[0]<=sample;
                for(k=1;k<291;k=k+1) history[k]<=history[k-1];
            end
        end
    end
    genvar t,s,j;
    generate for(t=0;t<512;t=t+1) begin:products
        if(t<292) begin
            localparam signed [17:0] C=coefficient(291-t);
            wire signed [17:0] value;
            if(t==0) assign value=sample;else assign value=history[t-1];
            reg signed [35:0] p;
            always @(posedge clk) if(reset) p<=0;else if(valid) p<=value*C;
            assign sums[t]={{8{p[35]}},p};
        end else assign sums[t]=44'sd0;
    end
    for(s=1;s<=9;s=s+1) begin:level
        localparam BASE=1024-(1024>>s),PREV=1024-(1024>>(s-1));
        for(j=0;j<(512>>s);j=j+1) begin:node
            reg signed [43:0] sum;
            always @(posedge clk) if(reset) sum<=0;else if(vp[s-1]) sum<=sums[PREV+2*j]+sums[PREV+2*j+1];
            assign sums[BASE+j]=sum;
        end
    end endgenerate
    wire signed [43:0] scaled=sums[1022]>>>12;
    assign out_sample=scaled>131071?18'sd131071:scaled< -131072?-18'sd131072:scaled[17:0];
    assign out_valid=vp[9];
endmodule

module p351_soq_detect(input clk,input reset,input valid,
    input signed [17:0] sample,input [17:0] unit,
    output reg bit_valid=0,output reg bit_data=0);
    // 四状态保存最近两个差分编码电平。两种起始奇偶可由状态变换等价表示。
    // 所有初始状态等可能；32位幸存路径，不使用已知PN载荷选路径。
    reg parity=0;reg [2:0] vp=0;reg [2:0] parity_pipe=0;
    reg signed [18:0] diff[0:2];reg [37:0] metric[0:2];
    reg signed [39:0] cost[0:3],chosen[0:3];
    reg [31:0] path[0:3],chosen_path[0:3];
    reg [7:0] seen=0;reg last_delta=0;
    reg signed [39:0] a,b;reg [1:0] best;
    integer j,p0,p1,m0,m1;
    always @(posedge clk) begin
        if(reset) begin
            parity<=0;vp<=0;parity_pipe<=0;seen<=0;last_delta<=0;bit_valid<=0;
            for(j=0;j<4;j=j+1) begin cost[j]<=0;path[j]<=0;end
        end else begin
            bit_valid<=0;vp<={vp[1:0],valid};parity_pipe<={parity_pipe[1:0],parity};
            if(valid) begin
                parity<=~parity;
                diff[0]<=$signed(sample)+$signed({1'b0,unit});
                diff[1]<=sample;diff[2]<=$signed(sample)-$signed({1'b0,unit});
            end
            if(vp[0]) for(j=0;j<3;j=j+1) metric[j]<=diff[j]*diff[j];
            if(vp[1]) begin
                for(j=0;j<4;j=j+1) begin
                    p0=(j>>1);p1=2+(j>>1);
                    // metric index: predicted alpha -1/0/+1 -> 0/1/2.
                    m0=1;m1=1;
                    if(j&1) m0=(parity_pipe[1] ^ ((j>>1)&1))?0:2;
                    else m1=(parity_pipe[1] ^ ((j>>1)&1))?2:0;
                    a=cost[p0]+$signed({2'b0,metric[m0]});
                    b=cost[p1]+$signed({2'b0,metric[m1]});
                    if(a<=b) begin chosen[j]<=a;chosen_path[j]<={path[p0][30:0],j[0]};end
                    else begin chosen[j]<=b;chosen_path[j]<={path[p1][30:0],j[0]};end
                end
            end
            if(vp[2]) begin
                best=chosen[0]<=chosen[1]?0:1;
                if(chosen[2]<chosen[best]) best=2;
                if(chosen[3]<chosen[best]) best=3;
                for(j=0;j<4;j=j+1) begin cost[j]<=chosen[j]-chosen[0];path[j]<=chosen_path[j];end
                if(seen<255) seen<=seen+1'b1;
                if(seen>=31) begin
                    // 同一条幸存路径上的相邻delta才能逆差分。
                    // 两条全局反相路径的代价相同，不能用上拍best的delta。
                    bit_data<=chosen_path[best][31]^chosen_path[best][30]^!parity_pipe[2];bit_valid<=1;
                end
                last_delta<=chosen_path[best][31];
            end
        end
    end
endmodule

module p351_rx_dsp #(parameter integer TRACKING_ENABLE=1)(input clk,input reset,input enable,input halt,input soq_mode,input randomized,
    input [2:0] line_code,input [5:0] sync_bits,input [8:0] major_count,input sfid_start,
    input [31:0] rate_word,input [23:0] resample_recip,input [32:0] sync_word,input [15:0] frame_bits,
    input adc_valid,input signed [11:0] adc_i,adc_q,
    output wire locked,output reg fault=0,
    output reg [4:0] scan_phase=0,
    output wire [31:0] bit_count,frame_count,payload_count,error_count,
    output wire stream_valid,stream_bit,output wire pn_aligned,output wire [17:0] sfid_status,
    output wire signed [31:0] timing_trim_q24,output wire [31:0] timing_rate_word);
    reg scan_reset=0;reg [15:0] scan_bits=0;
    reg cadence_seen=0;reg [2:0] gap=0;
    wire internal_reset=reset || !enable || halt || fault || scan_reset;
    wire obs_valid;wire signed [31:0] obs_angle;
    p351_rx_discriminator discriminator(clk,internal_reset,adc_valid,adc_i,adc_q,obs_valid,obs_angle);
    reg [31:0] sample_phase=0;
    wire [23:0] timing_recip;
    wire [33:0] phase_sum={2'b0,sample_phase}+{timing_rate_word,2'b0};
    reg s4_valid=0,extra=0;reg signed [17:0] s4_sample=0;
    reg [31:0] extra_phase=0,last_sample_phase=0,absolute_phase=0;reg phase_seen=0;
    reg signed [29:0] dc=0;
    reg [31:0] prev0=0,prev1=0,prev2=0;
    reg signed [31:0] difference0=0,difference1=0;
    reg [33:0] numerator0=0,numerator1=0;
    reg [57:0] fraction0=0,fraction1=0;
    reg [23:0] reciprocal0=0;
    reg signed [49:0] interpolation0=0,interpolation1=0;
    reg [1:0] count0=0,count1=0,count2=0;
    wire [31:0] target0=prev2+(interpolation0>>>16),target1=prev2+(interpolation1>>>16);
    wire signed [31:0] delta0=target0-last_sample_phase,delta1=extra_phase-last_sample_phase;
    wire signed [29:0] value0=(delta0>>>15)-$signed(dc>>>12),value1=(delta1>>>15)-$signed(dc>>>12);
    always @(posedge clk) begin
        if(internal_reset) begin
            sample_phase<={scan_phase[2:0],29'd0};s4_valid<=0;extra<=0;dc<=0;
            absolute_phase<=0;last_sample_phase<=0;phase_seen<=0;count0<=0;count1<=0;count2<=0;
            reciprocal0<=resample_recip;
        end else begin
            s4_valid<=0;count0<=0;count1<=count0;count2<=count1;
            if(extra) begin
                s4_valid<=phase_seen;s4_sample<=value1[17:0];last_sample_phase<=extra_phase;phase_seen<=1;extra<=0;
            end
            if(obs_valid) begin
                sample_phase<=phase_sum[31:0];
                count0<=phase_sum[33:32];
                numerator0<=34'd4294967296-{2'd0,sample_phase};
                numerator1<=34'd8589934592-{2'd0,sample_phase};
                difference0<=obs_angle;prev0<=absolute_phase;absolute_phase<=absolute_phase+obs_angle;
                reciprocal0<=timing_recip;
            end
            if(count0!=0) begin
                fraction0<=numerator0*reciprocal0;fraction1<=numerator1*reciprocal0;
                difference1<=difference0;prev1<=prev0;
            end
            if(count1!=0) begin
                interpolation0<=difference1*$signed({1'b0,fraction0[48:32]});
                interpolation1<=difference1*$signed({1'b0,fraction1[48:32]});prev2<=prev1;
            end
            if(count2!=0) begin
                s4_valid<=phase_seen;s4_sample<=value0[17:0];last_sample_phase<=target0;phase_seen<=1;
                extra_phase<=target1;extra<=count2==2;
            end
            if(s4_valid) dc<=dc+$signed(s4_sample);
            // Q12 mean update by 1/4096 at 4sps: fixed ~1024-bit time constant.
        end
    end
    wire fir_valid;wire signed [17:0] fir_sample;
    p351_soq_fir eq_filter(clk,internal_reset,s4_valid && soq_mode,s4_sample,fir_valid,fir_sample);
    reg [1:0] pcm_phase=0,soq_phase=0;
    reg signed [17:0] previous_sample=0,half_history1=0,half_history2=0;
    reg previous_level=0,previous_half=0,line_seen=0;
    reg pcm_valid=0,pcm_bit=0,soq_symbol_valid=0;
    reg signed [17:0] soq_symbol=0;
    wire signed [18:0] pcm_sum=$signed(previous_sample)+$signed(s4_sample);
    wire signed [18:0] first_half=$signed(half_history2)+$signed(half_history1);
    wire first_level=!first_half[18],second_level=!pcm_sum[18];
    wire biphase=line_code>=3 && line_code<=5;
    always @(posedge clk) begin
        if(internal_reset) begin
            pcm_phase<=0;soq_phase<=0;pcm_valid<=0;soq_symbol_valid<=0;previous_sample<=0;
            half_history1<=0;half_history2<=0;previous_level<=0;previous_half<=0;line_seen<=0;
        end
        else begin
            pcm_valid<=0;soq_symbol_valid<=0;
            if(s4_valid) begin
                previous_sample<=s4_sample;half_history1<=previous_sample;half_history2<=half_history1;
                pcm_phase<=pcm_phase+1'b1;
                if(!soq_mode && pcm_phase==scan_phase[4:3]) begin
                    // 每位四个重采样点；双相码的两半位分别积分判决。
                    // M/S用相邻电平或位边界跳变逆差分，不依赖PN内容。
                    pcm_valid<=line_seen || line_code==0 || line_code==3 || line_code==6;
                    case(line_code)
                        1:pcm_bit<=second_level^previous_level;
                        2:pcm_bit<=!(second_level^previous_level);
                        3:pcm_bit<=first_level;
                        4:pcm_bit<=!(first_level^previous_half);
                        5:pcm_bit<=first_level^previous_half;
                        default:pcm_bit<=second_level;
                    endcase
                    previous_level<=second_level;previous_half<=second_level;line_seen<=1;
                end
            end
            if(fir_valid) begin
                soq_phase<=soq_phase+1'b1;
                if(soq_phase==scan_phase[4:3]) begin soq_symbol_valid<=1;soq_symbol<=fir_sample;end
            end
        end
    end
    wire soq_bit_valid,soq_bit;
    p351_rx_timing #(.TRACKING_ENABLE(TRACKING_ENABLE)) timing_loop(
        .clk(clk),.reset(internal_reset),.soq_mode(soq_mode),.biphase(biphase),.symbol_phase(scan_phase[4:3]),
        .nominal_rate(rate_word),.nominal_recip(resample_recip),.sample_valid(s4_valid),.sample(s4_sample),
        .soq_valid(soq_symbol_valid),.soq_sample(soq_symbol),.adjusted_rate(timing_rate_word),
        .adjusted_recip(timing_recip),.trim_q24(timing_trim_q24));
    // 相位在4sps边界差分，单位固定32768；低码率不丢弃中间ADC样本。
    wire [17:0] detector_unit=18'd32768;
    p351_soq_detect detector(clk,internal_reset,soq_symbol_valid,soq_symbol,detector_unit,soq_bit_valid,soq_bit);
    wire raw_valid=soq_mode?soq_bit_valid:pcm_valid;
    wire raw_bit=soq_mode?soq_bit:pcm_bit;
    reg [14:0] random_history=0;reg [4:0] random_warm=0;
    reg clean_valid=0,clean_bit=0;
    always @(posedge clk) begin
        if(internal_reset) begin random_history<=0;random_warm<=0;clean_valid<=0;end
        else begin
            clean_valid<=0;
            if(raw_valid) begin
                random_history<={random_history[13:0],raw_bit};
                if(random_warm<15) random_warm<=random_warm+1'b1;
                clean_valid<=!randomized || random_warm>=15;
                clean_bit<=randomized?(raw_bit^random_history[13]^random_history[14]):raw_bit;
            end
        end
    end
    wire frame_fault;
    p351_rx_frames framing(.clk(clk),.reset(reset || !enable),.search_reset(scan_reset),.halt(halt),
        .valid(clean_valid && !fault),.bit_data(clean_bit),.sync_word(sync_word),.sync_bits(sync_bits),
        .frame_bits(frame_bits),.major_count(major_count),.sfid_start(sfid_start),
        .locked(locked),.fault(frame_fault),.pn_aligned(pn_aligned),.sfid_status(sfid_status),
        .bit_count(bit_count),.frame_count(frame_count),.payload_count(payload_count),.error_count(error_count),
        .stream_valid(stream_valid),.stream_bit(stream_bit));
    wire [17:0] scan_limit={2'b0,frame_bits}+({2'b0,frame_bits}<<1)+18'd256;
    // 数字锁帧只证明同步字间距正确；PN比较不参与相位/路径选择。
    always @(posedge clk) begin
        scan_reset<=0;
        if(reset || !enable) begin
            cadence_seen<=0;gap<=0;fault<=0;scan_phase<=0;scan_bits<=0;
        end else if(halt) begin end
        else if(!fault) begin
            if(frame_fault)fault<=1;
            if(adc_valid) begin
                if(cadence_seen && gap!=1) fault<=1;
                cadence_seen<=1;gap<=0;
            end else if(cadence_seen) begin
                if(gap>=2) fault<=1;
                else gap<=gap+1'b1;
            end
            if(scan_reset) scan_bits<=0;
            else if(clean_valid) begin
                if(!locked) begin
                    if({2'b0,scan_bits}>=scan_limit) begin
                        // 先搜索四个象限，再细化1/32 bit相位，不借助PN误码率。
                        scan_phase<=scan_phase[4:3]==3?{2'b0,scan_phase[2:0]+3'd1}:
                            {scan_phase[4:3]+2'd1,scan_phase[2:0]};
                        scan_reset<=1;scan_bits<=0;
                    end else scan_bits<=scan_bits+1'b1;
                end
            end
        end
    end
endmodule

// 小帧/ID型复帧解同步。PN比较仅用于测试统计，不参与锁帧判决。
// 载荷内的同步字被忽略；锁定后只在精确的下一小帧边界检查。
module p351_rx_frames(input clk,input reset,input search_reset,input halt,input valid,input bit_data,
    input [32:0] sync_word,input [5:0] sync_bits,input [15:0] frame_bits,
    input [8:0] major_count,input sfid_start,
    output reg locked=0,output reg fault=0,output reg pn_aligned=0,
    output wire [17:0] sfid_status,
    output reg [31:0] bit_count=0,frame_count=0,payload_count=0,error_count=0,
    output reg stream_valid=0,output reg stream_bit=0);
    reg [32:0] shift=0;
    reg seen=0,polarity=0,pending_lock=0,have_id=0;
    reg [15:0] distance=0,payload_left=0;
    reg [3:0] id_left=0;
    reg [7:0] id_shift=0,last_id=0;
    reg [8:0] expected_id=0;
    reg [10:0] pn=11'h7ff;
    wire major=major_count>1;
    wire [32:0] mask=33'h1ffffffff>>(33-sync_bits);
    wire [32:0] next_shift={shift[31:0],bit_data};
    wire hit=((next_shift^sync_word)&mask)==0 || ((next_shift^~sync_word)&mask)==0;
    wire hit_polarity=(((next_shift^sync_word)&mask)!=0);
    wire corrected=bit_data^polarity;
    wire [7:0] next_id={id_shift[6:0],corrected};
    wire id_range={1'b0,next_id}>={8'd0,sfid_start} &&
        {1'b0,next_id}<major_count+sfid_start;
    assign sfid_status={expected_id,pn_aligned,last_id};
    always @(posedge clk) begin
        stream_valid<=0;
        if(reset)begin
            locked<=0;fault<=0;pn_aligned<=0;shift<=0;seen<=0;polarity<=0;pending_lock<=0;have_id<=0;
            distance<=0;payload_left<=0;id_left<=0;id_shift<=0;last_id<=0;expected_id<=0;pn<=11'h7ff;
            bit_count<=0;frame_count<=0;payload_count<=0;error_count<=0;
        end else if(search_reset)begin
            // 搜索相位重置不是新任务；计数不能下降，否则PS误认为32位回绕。
            locked<=0;pn_aligned<=0;shift<=0;seen<=0;polarity<=0;pending_lock<=0;have_id<=0;
            distance<=0;payload_left<=0;id_left<=0;id_shift<=0;last_id<=0;expected_id<=0;pn<=11'h7ff;
        end else if(halt)begin locked<=0;pn_aligned<=0;end
        else if(valid && !fault)begin
            bit_count<=bit_count+1'b1;shift<=next_shift;
            if(seen)distance<=distance+1'b1;
            if(locked)begin stream_valid<=1;stream_bit<=corrected;end
            if((locked && distance==frame_bits-1) || (!locked && hit))begin
                if(locked && (!hit || polarity!=hit_polarity))begin fault<=1;locked<=0;pn_aligned<=0;end
                else begin
                    pending_lock<=seen && distance==frame_bits-1 && polarity==hit_polarity;
                    if(!seen || distance!=frame_bits-1 || polarity!=hit_polarity)begin
                        have_id<=0;pn_aligned<=0;
                    end
                    seen<=1;polarity<=hit_polarity;distance<=0;
                    if(major)begin id_left<=8;id_shift<=0;payload_left<=0;end
                    else if(locked || (seen && distance==frame_bits-1 && polarity==hit_polarity))begin
                        locked<=1;pn_aligned<=1;pn<=11'h7ff;payload_left<=frame_bits-sync_bits;
                    end
                end
            end else if(id_left!=0)begin
                id_shift<=next_id;id_left<=id_left-1'b1;
                if(id_left==1)begin
                    if(!id_range || (have_id && next_id!=expected_id[7:0]))begin
                        if(locked)begin fault<=1;locked<=0;pn_aligned<=0;end
                        else begin have_id<=0;seen<=0;pending_lock<=0;pn_aligned<=0;end
                    end else begin
                        have_id<=1;last_id<=next_id;
                        expected_id<=({1'b0,next_id}+1==major_count+sfid_start)?{8'd0,sfid_start}:{1'b0,next_id}+1'b1;
                        if(pending_lock && have_id)locked<=1;
                        if(locked || (pending_lock && have_id))begin
                            payload_left<=frame_bits-sync_bits-8;
                            if(next_id=={7'd0,sfid_start})begin pn<=11'h7ff;pn_aligned<=1;end
                        end
                    end
                end
            end else if(locked && payload_left!=0)begin
                payload_left<=payload_left-1'b1;
                if(pn_aligned)begin
                    payload_count<=payload_count+1'b1;
                    if(corrected!=pn[0])error_count<=error_count+1'b1;
                    pn<={pn[0]^pn[2],pn[10:1]};
                end
                if(payload_left==1)frame_count<=frame_count+1'b1;
            end
        end
    end
endmodule
