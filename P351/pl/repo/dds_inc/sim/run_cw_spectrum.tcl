set root [file normalize ./]
set work [file normalize ./tmp/cw_spectrum_r41]
create_project -force cw_spectrum_r41 $work -part xc7z020clg484-2
add_files -norecurse $root/repo/dds_inc/dds_inc.v
add_files -fileset sim_1 -norecurse $root/repo/dds_inc/sim/tb_cw_spectrum.sv
set_property top tb_cw_spectrum [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
