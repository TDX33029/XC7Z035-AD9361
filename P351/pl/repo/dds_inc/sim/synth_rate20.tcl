set root [file normalize .]
set out $root/output/rate20_engineering_20260921
file mkdir $out
create_project -in_memory -part xc7z020clg484-2
read_verilog [list $root/repo/dds_inc/dds_inc.v $root/repo/dds_inc/pcm_fm_modulator.v $root/repo/dds_inc/soqpsk_tg_modulator.v $root/repo/dds_inc/irig_randomizer.v]
synth_design -top dds_inc -part xc7z020clg484-2 -mode out_of_context
create_clock -period 16.276 -name sample_clk [get_ports clk]
report_utilization -hierarchical -file $out/dds_utilization.rpt
report_timing_summary -file $out/dds_synth_timing.rpt
write_checkpoint -force $out/dds_synth.dcp
opt_design
place_design
phys_opt_design
route_design
report_utilization -file $out/dds_routed_utilization.rpt
report_timing_summary -file $out/dds_routed_timing.rpt
write_checkpoint -force $out/dds_routed.dcp
if {[get_property SLACK [get_timing_paths -delay_type max -max_paths 1]] < 0} {error "OOC setup timing failed"}
puts "PASS waveform OOC implementation complete (not full system timing)"
