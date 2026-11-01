`timescale 1ns/1ps
module tb_pn9_word_checker;
reg clk=0,rst=1,clear=0,valid=0;
reg [11:0] data_i=0,data_q=0;
wire locked; wire [1:0] mode; wire [5:0] errors; wire oos;
pn9_word_checker dut(.clk(clk),.rst(rst),.clear(clear),.valid(valid),
 .data_i(data_i),.data_q(data_q),.locked(locked),
 .selected_mode(mode),.error_bits(errors),.oos_pulse(oos));
always #2 clk=~clk;

reg [15:0] state=16'h1ace;
reg [23:0] standard_state=24'h5a37c1;
reg [11:0] qrev;
integer i;

function [15:0] advance;
  input [15:0] din;
  begin advance={din[14:0],((^din[15:4])^(^din[2:1]))}; end
endfunction

function [23:0] standard_advance;
  input [23:0] din;
  reg [8:0] history;
  reg next_bit;
  integer n;
  begin
    history=din[8:0]; standard_advance=0;
    for(n=0;n<24;n=n+1) begin
      next_bit=history[8]^history[4];
      standard_advance[23-n]=next_bit;
      history={history[7:0],next_bit};
    end
  end
endfunction

function [11:0] reverse12;
  input [11:0] din;
  integer n;
  begin for(n=0;n<12;n=n+1) reverse12[11-n]=din[n]; end
endfunction

task send_state;
  input inject_error;
  begin
    // Actual I/Q meaning after the P351 UG570 pin-order repair.
    data_i=state[15:4];
    qrev={data_i[7:0],state[3:0]};
    data_q=reverse12(qrev);
    if(inject_error) data_i=data_i^12'h001;
    @(negedge clk); valid=1;
    @(negedge clk); valid=0;
    state=advance(state);
  end
endtask

task send_standard_word;
  begin
    data_i=standard_state[23:12]; data_q=0;
    @(negedge clk); valid=1; @(negedge clk); valid=0;
    data_i=standard_state[11:0];
    @(negedge clk); valid=1; @(negedge clk); valid=0;
    standard_state=standard_advance(standard_state);
  end
endtask

initial begin
  #20 rst=0;
  for(i=0;i<80;i=i+1) send_state(0);
  if(!locked || mode!=0) $fatal(1,"AD9361 PRBS failed to lock mode=%0d",mode);
  send_state(1);
  if(errors==0) $fatal(1,"Injected error not detected");
  @(negedge clk); clear=1; @(negedge clk); clear=0;
  for(i=0;i<80;i=i+1) send_standard_word;
  if(!locked || mode!=1) $fatal(1,"Standard PN9 failed to lock mode=%0d",mode);
  $display("TB_PASS device and standard modes");
  $finish;
end
endmodule
