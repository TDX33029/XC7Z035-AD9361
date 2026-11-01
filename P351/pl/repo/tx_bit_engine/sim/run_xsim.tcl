create_project -force tx_bit_engine_sim ./tmp/tx_bit_engine_sim -part xc7z020clg484-2
add_files -norecurse [file normalize ./repo/tx_bit_engine/src/tx_bit_engine_axis.v]
add_files -fileset sim_1 -norecurse [file normalize ./repo/tx_bit_engine/sim/tb_tx_bit_engine_axis.sv]
set_property top tb_tx_bit_engine_axis [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
