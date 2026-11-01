`timescale 1ns/1ps
// Bit-exact CW generator -> async sample queue -> physical DDR pin decoder.
module tb_cw_lvds_end_to_end;
  reg dc=0,sc=0,rst=1,locked=0;
  reg [3:0] mode=0;
  // 1R1T: 122.88 MHz DATA_CLK and 61.44 MHz sample clock.
  always #4.069010416667 dc=~dc;
  // The hardware sample clock is derived from DATA_CLK, not an independent
  // oscillator. Derive it here too: independent #delay rounding at 1 ps
  // creates a false frequency offset and eventually drains the FIFO.
  initial forever begin repeat(2) @(dc); #0.7; sc=~sc; end
  wire valid,cp,cn,fp,fn;
  wire [5:0] dp,dn;
  wire [11:0] li,lq,ci,cq,selected_i,selected_q;
  tx_waveform_channel wave(sc,rst,mode,3'd0,valid,li,lq,ci,cq);
  tx_waveform_mux mux(.mode(mode),.legacy_i(li),.legacy_q(lq),
    .cw_i(ci),.cw_q(cq),.soqpsk_i(12'd0),.soqpsk_q(12'd0),
    .pcm_i(12'd0),.pcm_q(12'd0),.selected_i(selected_i),.selected_q(selected_q));
  ad9361_lvds_tx dut(.rst(rst),.data_clk(dc),.sample_clk(sc),
    .sample_locked(locked),.dac_valid(valid),
    .dac_data_i0(selected_i),.dac_data_q0(selected_q),
    .dac_data_i1(12'd0),.dac_data_q1(12'd0),
    .tx_clk_out_p(cp),.tx_clk_out_n(cn),.tx_frame_out_p(fp),
    .tx_frame_out_n(fn),.tx_data_out_p(dp),.tx_data_out_n(dn));
  reg [23:0] expected[0:65535];
  integer wr=0,rd=0,slot=0,total=0,fd;
  reg aligned=0,previous_frame=0;
  reg [11:0] i0,q0;
  always @(posedge sc) begin
    if(rst || !locked) wr=0;
    else if(dut.sample_ready && valid && !dut.queue_full) begin
      expected[wr]={mode==6?ci:12'd0,mode==6?cq:12'd0};
      wr=wr+1;
    end
  end
  initial forever begin
    @(posedge dc); #0.2;
    if(rst || !locked) begin aligned=0;previous_frame=0;rd=0;end
    else begin
      if(!previous_frame && fp && !aligned) begin aligned=1;slot=0;end
      if(aligned) begin
        if(fp !== (slot==0)) $fatal(1,"CW 1R1T FRAME error");
        case(slot)
          0:i0[11:6]=dp;1:i0[5:0]=dp;
        endcase
      end
      @(negedge dc); #0.2;
      if(aligned) begin
        case(slot)
          0:q0[11:6]=dp;1:q0[5:0]=dp;
        endcase
        if(slot==1) begin
          if(rd>=wr || {i0,q0} !== expected[rd])
            $fatal(1,"CW pin data mismatch sample=%0d actual=%h expected=%h",rd,{i0,q0},expected[rd]);
          if(dut.overflow_sticky || dut.underflow_sticky) $fatal(1,"CW queue fault");
          rd=rd+1;total=total+1;
          if(total>128 && total<=49280) $fwrite(fd,"%0d %0d\n",$signed(i0),$signed(q0));
        end
        slot=(slot+1)%2;
      end
      previous_frame=fp;
    end
  end
  initial begin
    fd=$fopen("cw_pin_samples.txt","w");
    #31 rst=0; mode=6; #11 locked=1;
    wait(total>=49280);$fclose(fd);
    repeat(20) begin
      @(negedge sc);mode=0;
      repeat(32) @(negedge sc);
      mode=6;repeat(128) @(negedge sc);
    end
    @(negedge dc);#0.5 locked=0;#70 locked=1;
    wait(rd>=2048);
    $display("PASS CW through LVDS pins: %0d complete samples, 20 OFF/CW switches, lock recovery",total);
    $finish;
  end
  initial begin #2000000; $fatal(1,"timeout");end
endmodule
