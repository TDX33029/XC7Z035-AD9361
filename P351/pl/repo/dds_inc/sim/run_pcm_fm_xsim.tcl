set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/pcm_fm_sim]
create_project -force pcm_fm_sim $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/pcm_fm_modulator.v
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_pcm_fm_modulator.sv
set_property top tb_pcm_fm_modulator [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
