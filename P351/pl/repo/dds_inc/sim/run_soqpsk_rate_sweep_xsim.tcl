set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/soqpsk_rate_sweep_sim]
create_project -force soqpsk_rate_sweep_sim $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/soqpsk_tg_modulator.v
add_files -fileset sim_1 -norecurse $root_dir/sim/tb_soqpsk_rate_sweep.sv
set_property top tb_soqpsk_rate_sweep [get_filesets sim_1]
set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
