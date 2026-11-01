`timescale 1ns/1ps
module tb_lvds_prbs_checker_core;
reg clk=0, rst=1, enable=0, clear=0, snapshot_req=0, adc_valid=0;
reg [31:0] window_samples=128;
reg [11:0] adc_data_i0=0,adc_data_q0=0,adc_data_i1=0,adc_data_q1=0;
wire snapshot_ack;
wire [31:0] status_snapshot,sample_snapshot;
wire [31:0] error_i0_snapshot,error_q0_snapshot,error_i1_snapshot,error_q1_snapshot;
wire [31:0] gap_error_snapshot,oos_snapshot;
wire [31:0] raw0_snapshot,raw1_snapshot,raw2_snapshot,raw3_snapshot,raw4_snapshot;

lvds_prbs_checker_core dut(
 .data_clk(clk),.rst(rst),.enable(enable),.clear(clear),
 .window_samples(window_samples),.snapshot_req(snapshot_req),.snapshot_ack(snapshot_ack),
 .adc_valid(adc_valid),.adc_data_i0(adc_data_i0),.adc_data_q0(adc_data_q0),
 .adc_data_i1(adc_data_i1),.adc_data_q1(adc_data_q1),
 .status_snapshot(status_snapshot),.sample_snapshot(sample_snapshot),
 .error_i0_snapshot(error_i0_snapshot),.error_q0_snapshot(error_q0_snapshot),
 .error_i1_snapshot(error_i1_snapshot),.error_q1_snapshot(error_q1_snapshot),
 .gap_error_snapshot(gap_error_snapshot),.oos_snapshot(oos_snapshot),
 .raw0_snapshot(raw0_snapshot),.raw1_snapshot(raw1_snapshot),
 .raw2_snapshot(raw2_snapshot),.raw3_snapshot(raw3_snapshot),
 .raw4_snapshot(raw4_snapshot));
always #4 clk=~clk;

reg [15:0] p0=16'h1ace,p1=16'h5a37;
reg [11:0] qrev0,qrev1;
integer n,k;

function [15:0] advance;
  input [15:0] din;
  begin advance={din[14:0],((^din[15:4])^(^din[2:1]))}; end
endfunction

function [11:0] reverse12;
  input [11:0] din;
  integer j;
  begin for(j=0;j<12;j=j+1) reverse12[11-j]=din[j]; end
endfunction

task make_pairs;
begin
  adc_data_i0=p0[15:4]; qrev0={adc_data_i0[7:0],p0[3:0]};
  adc_data_q0=reverse12(qrev0);
  adc_data_i1=p1[15:4]; qrev1={adc_data_i1[7:0],p1[3:0]};
  adc_data_q1=reverse12(qrev1);
  p0=advance(p0); p1=advance(p1);
end
endtask

initial begin
  repeat(5) @(posedge clk); rst=0;
  @(negedge clk); enable=1;
  repeat(3) @(posedge clk);
  for(n=0;n<128;n=n+1) begin
    make_pairs;
    @(negedge clk); adc_valid=1;
    @(negedge clk); adc_valid=0;
    @(posedge clk);
  end
  repeat(3) @(posedge clk);
  snapshot_req=1;
  wait(snapshot_ack==1);
  #1;
  if(status_snapshot[1:0] != 2'b10) $fatal(1,"done/busy wrong: %h",status_snapshot);
  if(status_snapshot[5:2] != 4'h3) $fatal(1,"RX1 I/Q not locked: %h",status_snapshot);
  if(status_snapshot[7:6] != 0) $fatal(1,"error flags set: %h",status_snapshot);
  if(sample_snapshot != 128) $fatal(1,"sample count %0d",sample_snapshot);
  if((error_i0_snapshot|error_q0_snapshot|error_i1_snapshot|error_q1_snapshot|
      gap_error_snapshot|oos_snapshot) != 0) $fatal(1,"unexpected errors");
  if(raw0_snapshot[23:12] !== 12'h1ac || raw0_snapshot[11:0] !== reverse12(12'hace))
    $fatal(1,"raw first I0 mismatch: %h",raw0_snapshot);
  // raw4 is the fifth consecutive RX0 sample (p0 after four advances).
  if(raw4_snapshot[11:0] !== 12'h373)
    $fatal(1,"raw fifth I0 mismatch: %h",raw4_snapshot);
  $display("CORE_TB_PASS samples=%0d status=%h",sample_snapshot,status_snapshot);
  $finish;
end
endmodule
