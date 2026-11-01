# Run from project root. Output is separate from historical audit evidence.
set root [file normalize .]
set out $root/output/irig_review_20260918
if {[info exists ::env(IRIG_REVIEW_OUT)]} {set out [file normalize $::env(IRIG_REVIEW_OUT)]}
file mkdir $out
foreach {name top sources} {
 encoding tb_irig_encoding_audit {repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/irig_randomizer.v repo/dds_inc/sim/tb_irig_encoding_audit.sv}
 guard tb_standard_rate_guard {repo/dds_inc/dds_inc.v repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/soqpsk_tg_modulator.v repo/dds_inc/irig_randomizer.v repo/dds_inc/sim/tb_standard_rate_guard.sv}
 pcm_rate tb_pcm_fm_rate_sweep {repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/sim/tb_pcm_fm_rate_sweep.sv}
 soqpsk tb_soqpsk_irig_audit {repo/dds_inc/soqpsk_tg_modulator.v repo/dds_inc/irig_randomizer.v reference_model/regression_baselines/soqpsk_tg_baseline.v repo/dds_inc/sim/tb_soqpsk_irig_audit.sv}
 word_boundary tb_word_boundary_audit {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_word_boundary_audit.sv}
 circular tb_tx_bit_engine_circular {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/tx_bit_engine/sim/tb_tx_bit_engine_circular.sv}
} {
 if {[llength $argv] && [lsearch -exact $argv $name] < 0} {continue}
 create_project -force $name $out/$name -part xc7z020clg484-2
 foreach src $sources {add_files -fileset sim_1 -norecurse $root/$src}
 set_property top $top [get_filesets sim_1]
 set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
 launch_simulation
 run all
 close_sim
 close_project
 # XSim $fatal can return control to Tcl without a failing process status.
 set logpath $out/$name/$name.sim/sim_1/behav/xsim/simulate.log
 set fd [open $logpath r]
 set result [read $fd]
 close $fd
 if {[regexp {Fatal:|FATAL:|ERROR:} $result] || ![string match {*$finish*} $result]} {
   error "Regression $name failed or did not finish; inspect $logpath"
 }
}
