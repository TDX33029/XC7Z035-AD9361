set root [file normalize .]
create_project -force prbs_1r1t [file join $root tmp prbs_1r1t] -part xc7z020clg484-2
add_files [file join $root repo lvds_prbs_checker src pn9_word_checker.v]
add_files [file join $root repo lvds_prbs_checker src lvds_prbs_checker_core.v]
add_files -fileset sim_1 [file join $root repo lvds_prbs_checker sim tb_lvds_prbs_checker_core.v]
set_property top tb_lvds_prbs_checker_core [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
