set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/standard_rate_guard_sim]
create_project -force standard_rate_guard_sim $work_dir -part xc7z020clg484-2
add_files -norecurse [list $root_dir/dds_inc.v $root_dir/pcm_fm_modulator.v \
    $root_dir/soqpsk_tg_modulator.v $root_dir/artm_cpm_modulator.v \
    $root_dir/irig_randomizer.v]
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_standard_rate_guard.sv
set_property top tb_standard_rate_guard [get_filesets sim_1]
set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
exit
