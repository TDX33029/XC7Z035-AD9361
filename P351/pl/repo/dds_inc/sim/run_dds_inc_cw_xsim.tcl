set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/dds_inc_cw_mux_r41]
create_project -force dds_inc_cw_sim $work_dir -part xc7z020clg484-2
add_files -norecurse [list $root_dir/dds_inc.v $root_dir/pcm_fm_modulator.v \
    $root_dir/soqpsk_tg_modulator.v \
    $root_dir/irig_randomizer.v]
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_dds_inc_cw.sv
set_property top tb_dds_inc_cw [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
