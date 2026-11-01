set root [file normalize .]
create_project -force soqpsk_audit $root/output/soqpsk_audit_20260908/sim -part xc7z020clg484-2
add_files -fileset sim_1 [list $root/repo/dds_inc/soqpsk_tg_modulator.v $root/repo/dds_inc/irig_randomizer.v $root/reference_model/regression_baselines/soqpsk_tg_baseline.v $root/repo/dds_inc/sim/tb_soqpsk_irig_audit.sv]
set_property top tb_soqpsk_irig_audit [get_filesets sim_1]
set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
