`timescale 1ns/1ps

module tb_soqpsk_rate_sweep;
    localparam [31:0] RATE_12M   = 32'h3200_0000;
    localparam [31:0] RATE_12P5M = 32'h3415_5555;
    localparam [31:0] RATE_15P36M = 32'h4000_0000;

    reg clk=0, rst=1, enable=0, clear=0;
    reg [31:0] rate_word=RATE_12M;
    reg [31:0] bit_phase=0;
    reg [14:0] lfsr=15'h0001;
    reg [1:0] mode=0;
    reg capture=0;
    wire [32:0] rate_sum={1'b0,bit_phase}+{1'b0,rate_word};
    wire bit_valid=!rst&&enable&&rate_sum[32];
    wire bit_data=lfsr[14];
    wire bit_ready,iq_valid;
    wire [11:0] data_i,data_q;
    wire [31:0] accepted_bits,debug_phase;
    wire signed [15:0] debug_alpha,debug_frequency;
    integer cycle=0,last12=-1,last125=-1,last1536=-1,events12=0,events125=0,events1536=0;
    integer saw5=0,saw6=0,saw4=0,saw5b=0,failures=0,fd;

    always #8.138020833 clk=~clk;

    soqpsk_tg_modulator dut(
        .clk(clk),.rst(rst),.enable(enable),.clear(clear),
        .differential_enable(1'b1),.bit_data(bit_data),.bit_valid(bit_valid),
        .bit_ready(bit_ready),.bit_phase(bit_phase),.bit_rate_word(rate_word),
        .frequency_offset_word(32'sd0),.amplitude(16'd26214),
        .data_i(data_i),.data_q(data_q),.iq_valid(iq_valid),
        .accepted_bits(accepted_bits),.debug_alpha(debug_alpha),
        .debug_frequency(debug_frequency),.debug_phase(debug_phase));

    always @(posedge clk) begin
        cycle=cycle+1;
        if(rst||clear||!enable) bit_phase<=0;
        else bit_phase<=rate_sum[31:0];
        if(bit_valid&&bit_ready) begin
            lfsr<={lfsr[13:0],lfsr[14]^lfsr[13]};
            if(mode==1) begin
                if(last12>=0) begin
                    if(cycle-last12==5)saw5=1;
                    else if(cycle-last12==6)saw6=1;
                    else begin $display("ERROR 12M interval=%0d",cycle-last12);failures=failures+1;end
                end
                last12=cycle;events12=events12+1;
            end else if(mode==2) begin
                if(last125>=0) begin
                    if(cycle-last125==4)saw4=1;
                    else if(cycle-last125==5)saw5b=1;
                    else begin $display("ERROR 12.5M interval=%0d",cycle-last125);failures=failures+1;end
                end
                last125=cycle;events125=events125+1;
            end else if(mode==3) begin
                if(last1536>=0 && cycle-last1536!=4)begin
                    $display("ERROR 15.36M interval=%0d",cycle-last1536);failures=failures+1;end
                last1536=cycle;events1536=events1536+1;
            end
        end
        if(capture&&iq_valid)$fwrite(fd,"%0d %0d\n",$signed(data_i),$signed(data_q));
    end

    initial begin
        repeat(12)@(posedge clk);rst<=0;enable<=1;mode<=1;
        repeat(8000)@(posedge clk);
        if(events12<1562||events12>1563||!saw5||!saw6)begin
            $display("ERROR 12M events=%0d saw5/6=%0d/%0d",events12,saw5,saw6);failures=failures+1;end
        clear<=1;@(posedge clk);clear<=0;rate_word<=RATE_12P5M;mode<=2;
        repeat(1000)@(posedge clk);
        fd=$fopen("soqpsk_12p5_iq.txt","w");capture<=1;
        repeat(65536)@(posedge clk);capture<=0;@(posedge clk);$fclose(fd);
        if(events125<13536||events125>13537||!saw4||!saw5b)begin
            $display("ERROR 12.5M events=%0d saw4/5=%0d/%0d",events125,saw4,saw5b);failures=failures+1;end
        clear<=1;@(posedge clk);clear<=0;rate_word<=RATE_15P36M;mode<=3;
        repeat(1000)@(posedge clk);
        fd=$fopen("soqpsk_15p36_iq.txt","w");capture<=1;
        repeat(65536)@(posedge clk);capture<=0;@(posedge clk);$fclose(fd);
        if(events1536<16633||events1536>16635)begin
            $display("ERROR 15.36M events=%0d",events1536);failures=failures+1;end
        if(failures==0)$display("PASS SOQPSK 12/12.5/15.36M timing; events=%0d/%0d/%0d",events12,events125,events1536);
        else $fatal(1,"SOQPSK rate sweep failures=%0d",failures);
        $finish;
    end
endmodule
