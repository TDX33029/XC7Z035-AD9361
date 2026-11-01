set out [file normalize ./tmp/cw_spectrum_r41]
file mkdir $out
read_verilog [list \
    [file normalize repo/dds_inc/dds_inc.v] \
    [file normalize repo/dds_inc/pcm_fm_modulator.v] \
    [file normalize repo/dds_inc/soqpsk_tg_modulator.v] \
    [file normalize repo/dds_inc/artm_cpm_modulator.v] \
    [file normalize repo/dds_inc/irig_randomizer.v]]
synth_design -top dds_inc -part xc7z020clg484-2 -mode out_of_context
create_clock -period 16.276 [get_ports clk]
report_utilization -file $out/dds_utilization.rpt
report_timing_summary -file $out/dds_timing.rpt
report_timing -from [get_cells -hier -filter {NAME =~ *cw_phase_reg*}] -max_paths 5 -file $out/cw_timing.rpt
exit
