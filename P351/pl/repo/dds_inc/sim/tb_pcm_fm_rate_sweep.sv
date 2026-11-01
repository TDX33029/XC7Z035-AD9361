`timescale 1ns/1ps

module tb_pcm_fm_rate_sweep;
    localparam [31:0] RATE_4P2  = 32'd293601280;
    localparam [31:0] DEV_4P2   = 32'd102760448;
    localparam [31:0] RATE_12P5 = 32'd873813333;
    localparam [31:0] DEV_12P5  = 32'd305834667;
    localparam [31:0] RATE_15P36 = 32'h4000_0000;
    localparam [31:0] DEV_15P36  = 32'h1666_6666;
    localparam [31:0] RATE_20M = 32'h5355_5555;
    localparam [31:0] DEV_20M  = 32'h1D2A_AAAB;

    reg clk=0, rst=1, enable=0, clear=0;
    reg bit_data=0;
    reg [31:0] bit_phase=0, rate_word=RATE_4P2, deviation_word=DEV_4P2;
    wire [32:0] phase_sum={1'b0,bit_phase}+{1'b0,rate_word};
    wire bit_valid=enable && !rst && !clear && phase_sum[32];
    wire bit_ready, iq_valid;
    wire [11:0] data_i, data_q;
    wire [31:0] accepted_bits, debug_phase;
    wire signed [15:0] debug_nrz, debug_filter;
    reg [14:0] lfsr=15'h1;
    integer interval=0, min_interval=999, max_interval=0, edges=0;
    integer capture_count=0, fd=0;
    reg capture=0;
    integer scan_index;
    real ideal_sine, scan_error, max_error=0.0, old_max_error=0.0;

    always #8.138020833 clk=~clk;

    pcm_fm_modulator dut(
        .clk(clk),.rst(rst),.enable(enable),.clear(clear),
        .bit_data(bit_data),.bit_valid(bit_valid),.bit_ready(bit_ready),
        .bit_phase(bit_phase),.bit_rate_word(rate_word),
        .deviation_word(deviation_word),.frequency_offset_word(32'sd0),
        .amplitude(16'd26214),.filter_profile(4'd0),
        .line_code(3'd0),.rolloff(3'd0),
        .data_i(data_i),.data_q(data_q),.iq_valid(iq_valid),
        .accepted_bits(accepted_bits),.debug_nrz(debug_nrz),
        .debug_filter(debug_filter),.debug_phase(debug_phase));

    always @(posedge clk) begin
        if (rst || clear || !enable) begin
            bit_phase<=0; interval<=0; min_interval<=999; max_interval<=0; edges<=0;
        end else begin
            bit_phase<=phase_sum[31:0]; interval<=interval+1;
            if (phase_sum[32]) begin
                lfsr<={lfsr[13:0],lfsr[14]^lfsr[13]};
                bit_data<=lfsr[14]^lfsr[13];
                if (edges>2) begin
                    if (interval<min_interval) min_interval<=interval;
                    if (interval>max_interval) max_interval<=interval;
                end
                interval<=0; edges<=edges+1;
            end
            if (capture && iq_valid) begin
                $fwrite(fd,"%0d %0d %0d\n",$signed(data_i),$signed(data_q),debug_filter);
                capture_count<=capture_count+1;
            end
        end
    end

    task restart;
        begin
            clear<=1; repeat(4) @(posedge clk); clear<=0;
            repeat(16) @(posedge clk);
        end
    endtask

    initial begin
        for(scan_index=0;scan_index<65536;scan_index=scan_index+1) begin
            ideal_sine=32767.0*$sin(6.283185307179586*scan_index/65536.0);
            scan_error=$signed(dut.sine_interpolated(scan_index[15:0]))-ideal_sine;
            if(scan_error<0) scan_error=-scan_error;
            if(scan_error>max_error) max_error=scan_error;
            scan_error=$signed(dut.sine_lut(scan_index[15:8]))-ideal_sine;
            if(scan_error<0) scan_error=-scan_error;
            if(scan_error>old_max_error) old_max_error=scan_error;
        end
        if(max_error>4.0) $fatal(1,"PCM sine interpolation inaccurate %f",max_error);
        $display("PASS PCM 65536-phase sine scan max_error=%f old=%f Q15 LSB",max_error,old_max_error);
        repeat(12) @(posedge clk); rst<=0; enable<=1;
        restart();
        repeat(10000) @(posedge clk);
        if (min_interval<13 || max_interval>14)
            $fatal(1,"4.2M interval failed min=%0d max=%0d",min_interval,max_interval);
        $display("PASS PCM/FM 4.2M fractional intervals=%0d/%0d",min_interval,max_interval);

        rate_word<=RATE_12P5; deviation_word<=DEV_12P5; restart();
        repeat(10000) @(posedge clk);
        if (min_interval<3 || max_interval>4)
            $fatal(1,"12.5M interval failed min=%0d max=%0d",min_interval,max_interval);
        $display("PASS PCM/FM 12.5M fractional intervals=%0d/%0d",min_interval,max_interval);
        fd=$fopen("pcm_fm_12p5_iq.txt","w"); capture<=1;
        repeat(131072) @(posedge clk);
        capture<=0; @(posedge clk); $fclose(fd);
        if (capture_count<120000) $fatal(1,"short capture %0d",capture_count);
        $display("PASS PCM/FM continuous-rate capture samples=%0d accepted=%0d",capture_count,accepted_bits);

        capture_count=0;
        rate_word<=RATE_15P36; deviation_word<=DEV_15P36; restart();
        repeat(10000) @(posedge clk);
        if (min_interval!=3 || max_interval!=3)
            $fatal(1,"15.36M interval failed min=%0d max=%0d",min_interval,max_interval);
        $display("PASS PCM/FM 15.36M exact-divisor rate interval=%0d",min_interval+1);
        fd=$fopen("pcm_fm_15p36_iq.txt","w"); capture<=1;
        repeat(131072) @(posedge clk);
        capture<=0; @(posedge clk); $fclose(fd);
        if (capture_count<120000) $fatal(1,"15.36M short capture %0d",capture_count);
        $display("PASS PCM/FM 15.36M capture samples=%0d accepted=%0d",capture_count,accepted_bits);
        rate_word<=RATE_20M; deviation_word<=DEV_20M; restart();
        repeat(10000) @(posedge clk);
        if (min_interval!=2 || max_interval!=3)
            $fatal(1,"20M interval failed min=%0d max=%0d",min_interval,max_interval);
        if (!iq_valid || accepted_bits<3000)
            $fatal(1,"20M modulation stalled accepted=%0d",accepted_bits);
        $display("PASS PCM/FM 20M fractional intervals=%0d/%0d accepted=%0d",
                 min_interval+1,max_interval+1,accepted_bits);
        $finish;
    end
endmodule
