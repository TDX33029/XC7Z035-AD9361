set root [file normalize .]
file mkdir $root/output/soqpsk_audit_20260908
foreach {name top src} {
 baseline soqpsk_tg_baseline reference_model/regression_baselines/soqpsk_tg_baseline.v
 fixed soqpsk_tg_modulator repo/dds_inc/soqpsk_tg_modulator.v
} {
 if {[llength $argv] && [lsearch -exact $argv $name]<0} {continue}
 create_project -in_memory -part xc7z020clg484-2
 read_verilog $root/$src
 synth_design -top $top -part xc7z020clg484-2 -mode out_of_context
 create_clock -name sample_clk -period 16.276041 [get_ports clk]
 # Existing top-level routed clock report places sample_clk at this BUFG.
 set_property HD.CLK_SRC BUFGCTRL_X0Y1 [get_ports clk]
 set_input_delay 0 -clock sample_clk [get_ports -filter {DIRECTION == IN && NAME != clk}]
 set_output_delay 0 -clock sample_clk [get_ports -filter {DIRECTION == OUT}]
 opt_design
 place_design
 route_design
 report_utilization -file $root/output/soqpsk_audit_20260908/${name}_utilization.rpt
 report_timing_summary -file $root/output/soqpsk_audit_20260908/${name}_timing.rpt
 close_project
}
