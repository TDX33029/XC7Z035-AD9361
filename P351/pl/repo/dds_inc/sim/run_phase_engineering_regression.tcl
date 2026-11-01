# Run from project root; no hardware connection, IP upgrade or bitstream write.
set root [file normalize .]
set out $root/output/rate20_engineering_20260921/regression
foreach {name top sources marker} {
 gain tb_pcm_gain_config {pcm_fm_modulator.v sim/tb_pcm_gain_config.sv} {PASS PCM ratio}
 codes tb_pcm_six_line_codes {pcm_fm_modulator.v sim/tb_pcm_six_line_codes.sv} {PASS PCM/FM seven}
 encoding tb_irig_encoding_audit {pcm_fm_modulator.v irig_randomizer.v sim/tb_irig_encoding_audit.sv} {AUDIT code=6}
 guard tb_standard_rate_guard {dds_inc.v pcm_fm_modulator.v soqpsk_tg_modulator.v irig_randomizer.v sim/tb_standard_rate_guard.sv} {PASS PL PCM/SOQPSK}
} {
 create_project -force $name $out/$name -part xc7z020clg484-2
 foreach src $sources {add_files -fileset sim_1 -norecurse $root/repo/dds_inc/$src}
 set_property top $top [get_filesets sim_1]
 set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
 launch_simulation
 run all
 close_sim
 close_project
 set f [open $out/$name/$name.sim/sim_1/behav/xsim/simulate.log r]
 set log [read $f];close $f
 if {[regexp {Fatal:|FATAL:|ERROR:} $log] || [string first $marker $log]<0} {error "Regression failed: $name"}
}
puts "PASS engineering gain, seven codes, IRIG encoding/randomizer, 20M guards"
