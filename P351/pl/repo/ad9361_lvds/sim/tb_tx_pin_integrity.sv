`timescale 1ns/1ps
// Decode actual ODDR/OBUFDS outputs, not pre-LVDS DAC monitor samples.
module tx_pin_case #(parameter integer ID=0)(output reg done=0);
  reg dc=0, sc=0, rst=1, locked=0;
  reg [11:0] seq=0;
  wire cp,cn,fp,fn;
  wire [5:0] dp,dn;
  always #4 dc=~dc;
  initial begin #(ID*0.5); forever #8 sc=~sc; end
  always @(negedge sc) if (rst || !locked) seq<=0; else seq<=seq+1;
  ad9361_lvds_tx dut(.rst(rst),.data_clk(dc),.sample_clk(sc),
    .sample_locked(locked),.dac_valid(1'b1),
    .dac_data_i0(seq),.dac_data_q0(seq ^ 12'haaa),
    .dac_data_i1(seq ^ 12'h555),.dac_data_q1(seq ^ 12'hfff),
    .tx_clk_out_p(cp),.tx_clk_out_n(cn),.tx_frame_out_p(fp),
    .tx_frame_out_n(fn),.tx_data_out_p(dp),.tx_data_out_n(dn));
  integer slot=0, frames=0;
  reg previous_frame=0, aligned=0;
  reg [11:0] i0,q0,previous_seq;
  reg saved_frame;
  initial forever begin
    @(posedge dc); #0.2;
    if (rst || !locked) begin
      aligned=0; previous_frame=0; frames=0;
    end else begin
      if (!previous_frame && fp && !aligned) begin aligned=1; slot=0; end
      saved_frame=fp;
      if (aligned) begin
        if (fp !== (slot==0)) $fatal(1,"FRAME length/order ID=%0d slot=%0d",ID,slot);
        case(slot)
          0:i0[11:6]=dp; 1:i0[5:0]=dp;
        endcase
      end
      @(negedge dc); #0.2;
      if (aligned) begin
        if (fp !== saved_frame || dp !== ~dn || cp !== ~cn)
          $fatal(1,"DDR frame/differential mismatch ID=%0d",ID);
        case(slot)
          0:q0[11:6]=dp; 1:q0[5:0]=dp;
        endcase
        if(slot==1) begin
          if(q0 !== (i0^12'haaa))
            $fatal(1,"TORN SAMPLE ID=%0d frame=%0d %h/%h",ID,frames,i0,q0);
          if(frames>0 && i0 !== ((previous_seq+1)&12'hfff))
            $fatal(1,"LOST/DUPLICATE ID=%0d previous=%h current=%h",ID,previous_seq,i0);
          previous_seq=i0; frames=frames+1;
          if(dut.overflow_sticky || dut.underflow_sticky)
            $fatal(1,"queue overflow/underflow ID=%0d",ID);
        end
        slot=(slot+1)%2;
      end
      previous_frame=saved_frame;
    end
  end
  initial begin
    #31 rst=0; #11 locked=1;
    wait(frames>=1024);
    // Stop between frames, verify fresh startup with no stale sample halves.
    @(negedge dc); #0.5 locked=0;
    #40 locked=1;
    wait(frames>=1024);
    done=1;
  end
  reg stress_done=0;
  // Digital stress model: one extra synchronizer cycle, NOT a simulation
  // of analog metastability or evidence this happens on the physical board.
  initial if(ID==0 && $test$plusargs("CDC_STRESS")) begin
    wait(frames>=100);
    @(negedge sc); @(posedge sc); #0.1;
    force dut.wr_gray_meta=dut.wr_gray_sync;
    @(posedge dc); #0.1; release dut.wr_gray_meta;
    stress_done=1;
  end
endmodule

module tb_tx_pin_integrity;
  wire [7:0] done;
  genvar n;
  generate for(n=0;n<8;n=n+1) begin:c
    tx_pin_case #(.ID(n)) t(done[n]);
  end endgenerate
  initial begin
    wait(&done);
    if ($test$plusargs("CDC_STRESS") && !c[0].t.stress_done)
      $fatal(1,"CDC stress injection did not run");
    $display("PASS: 8 clock phases, 2048 frames each, TX1 I/Q decoded, lock recovery");
    $finish;
  end
  initial begin #100000; $fatal(1,"timeout"); end
endmodule
