`timescale 1ns/1ps

module tb_standard_rate_guard;
    reg clk=0, rst=1;
    reg [31:0] phase_inc=0, pcm_control=0, rate_word=0;
    wire bit_ready;
    wire [11:0] i0,q0,i1,q1;
    reg [31:0] offset_word=0;
    reg [15:0] amp=26214;
    reg [3:0] profile=0;
    reg bad_deviation=0;
    wire [63:0] canonical_dev=({32'd0,rate_word}*64'd35+64'd50)/64'd100;

    always #8.138020833 clk=~clk;

    dds_inc dut(
        .clk(clk),.rst(rst),.phase_inc(phase_inc),
        .bit_data(1'b0),.bit_valid(1'b0),.bit_phase(32'd0),
        .frame_start(1'b0),.frame_end(1'b0),.bit_ready(bit_ready),
        .pcm_deviation_word(canonical_dev[31:0]+(bad_deviation ? 32'd1:32'd0)),.pcm_frequency_offset(offset_word),
        .pcm_amplitude(amp),.pcm_filter_profile(profile),
        .pcm_control(pcm_control),.soqpsk_bit_rate_word(rate_word),
        .artm_bit_rate_word(32'd0),.pcm_iq_valid(),.pcm_accepted_bits(),
        .pcm_debug_nrz(),.pcm_debug_filter(),.pcm_debug_phase(),
        .dac_valid(),.dac_data_i0(i0),.dac_data_q0(q0),
        .dac_data_i1(i1),.dac_data_q1(q1));

    task settle_and_expect;
        input expected;
        input [127:0] label_text;
        begin
            // PCM gain configuration is serial: allow CDC + 64 divider clocks.
            repeat(80) @(posedge clk);
            if(bit_ready!==expected)
                $fatal(1,"%s expected bit_ready=%0d got=%0d",label_text,expected,bit_ready);
        end
    endtask

    initial begin
        repeat(6) @(posedge clk); rst=0;
        // Keep the directed checks beyond Vivado's default 1000 ns launch
        // window so the following Tcl "run all" owns the final $finish.
        repeat(40) @(posedge clk);
        phase_inc=32'h0000_0004; pcm_control=32'h1;
        rate_word=32'h000A_AAAA; settle_and_expect(0,"below minimum PCM");
        rate_word=32'h000A_AAAB; settle_and_expect(1,"minimum PCM");
        rate_word=32'h5355_5555; settle_and_expect(1,"maximum PCM");
        rate_word=32'h5355_5556; settle_and_expect(0,"above maximum PCM");
        rate_word=32'h14d55555;
        pcm_control=32'h61; settle_and_expect(0,"RNRZ missing IRIG15");
        pcm_control=32'h69; settle_and_expect(1,"RNRZ with IRIG15");
        pcm_control=32'h71; settle_and_expect(0,"reserved PCM code");
        pcm_control=32'h09; settle_and_expect(0,"NRZ stray IRIG15");
        pcm_control=32'h51; settle_and_expect(1,"Biphase compatibility");
        bad_deviation=1; settle_and_expect(0,"wrong PCM h");
        bad_deviation=0; settle_and_expect(1,"restored PCM h");
        offset_word=1; settle_and_expect(0,"PCM offset");
        offset_word=0; amp=16'h8000; settle_and_expect(0,"PCM amplitude");
        amp=26214; profile=1; settle_and_expect(0,"PCM profile");
        profile=0;
        phase_inc=32'h0000_0003;
        pcm_control=32'h1;
        rate_word=32'h5355_5555; settle_and_expect(0,"SOQ missing coding");
        pcm_control=32'h5; settle_and_expect(0,"SOQ missing IRIG15");
        pcm_control=32'h9; settle_and_expect(0,"SOQ missing diff");
        pcm_control=32'hD; settle_and_expect(1,"maximum SOQPSK");
        offset_word=1; settle_and_expect(0,"SOQ offset");
        offset_word=0; amp=16'h8000; settle_and_expect(0,"SOQ amplitude");
        amp=26214; profile=1; settle_and_expect(0,"SOQ profile");
        profile=0; settle_and_expect(1,"restored SOQ");
        rate_word=32'h5355_5556; settle_and_expect(0,"above maximum SOQPSK");
        $display("PASS PL PCM/SOQPSK rate guard and SOQ differential/IRIG15 guard");
        $finish;
    end
endmodule
