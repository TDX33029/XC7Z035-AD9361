set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/artm_cpm_sim]
create_project -force artm_cpm_sim $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/artm_cpm_modulator.v
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_artm_cpm_modulator.sv
set_property top tb_artm_cpm_modulator [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
