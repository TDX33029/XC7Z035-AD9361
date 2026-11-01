set root [file normalize .]
set out $root/output/rate20_engineering_20260921
file mkdir $out
create_project -force rate20 $out/sim -part xc7z020clg484-2
foreach src {repo/tx_bit_engine/src/tx_bit_engine_axis.v repo/dds_inc/irig_randomizer.v repo/dds_inc/pcm_fm_modulator.v repo/dds_inc/soqpsk_tg_modulator.v repo/dds_inc/sim/tb_rate20.sv} {
 add_files -fileset sim_1 -norecurse $root/$src
}
set_property top rate20_tb [get_filesets sim_1]
set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
set f [open $out/sim/rate20.sim/sim_1/behav/xsim/simulate.log r]
set result [read $f];close $f
if {![string match {*PASS all feasibility cases*} $result] || [regexp {Fatal:|FATAL:|ERROR:} $result]} {error "feasibility failed"}
