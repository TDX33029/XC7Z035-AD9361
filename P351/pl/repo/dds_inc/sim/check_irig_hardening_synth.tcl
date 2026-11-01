set root [file normalize .]
set out $root/output/irig_hardening_20260918/synthesis
file mkdir $out
create_project -in_memory -part xc7z020clg484-2
read_verilog [list $root/repo/dds_inc/dds_inc.v $root/repo/dds_inc/pcm_fm_modulator.v $root/repo/dds_inc/soqpsk_tg_modulator.v $root/repo/dds_inc/irig_randomizer.v]
synth_design -top dds_inc -mode out_of_context -part xc7z020clg484-2
create_clock -period 16.276041 -name sample_clk [get_ports clk]
report_utilization -file $out/utilization.rpt
report_timing_summary -file $out/timing_synthesis.rpt
write_checkpoint -force $out/dds_inc.dcp
# Synthesis only: this does NOT sign off placed/routed timing or board I/O.
