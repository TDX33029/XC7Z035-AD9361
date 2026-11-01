set root [file normalize .]
set selected $argv
foreach {name top sources} {
 encoding tb_irig_encoding_audit {repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/irig_randomizer.v repo/dds_inc/sim/tb_irig_encoding_audit.sv}
 word_boundary tb_word_boundary_audit {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_word_boundary_audit.sv}
 axis tb_tx_bit_engine_axis {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_tx_bit_engine_axis.sv}
 circular tb_tx_bit_engine_circular {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_tx_bit_engine_circular.sv}
 major20 tb_pcm_major_frame_20m {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_pcm_major_frame_20m.sv}
 pcm_codes tb_pcm_six_line_codes {repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/sim/tb_pcm_six_line_codes.sv}
 pcm_rate tb_pcm_fm_rate_sweep {repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/sim/tb_pcm_fm_rate_sweep.sv}
} {
 if {[llength $selected] && [lsearch -exact $selected $name] < 0} {continue}
 create_project -force $name $root/output/pcm_fix_20260907/$name -part xc7z020clg484-2
 foreach src $sources {add_files -fileset sim_1 -norecurse $root/$src}
 set_property top $top [get_filesets sim_1]
 set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
 launch_simulation
 run all
 close_sim
 close_project
}
