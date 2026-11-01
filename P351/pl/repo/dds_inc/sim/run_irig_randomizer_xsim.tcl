set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/irig_randomizer]
create_project -force irig_randomizer $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/irig_randomizer.v
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_irig_randomizer.sv
set_property top tb_irig_randomizer [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
