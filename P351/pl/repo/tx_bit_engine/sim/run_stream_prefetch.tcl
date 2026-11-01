set root [file normalize .]
create_project -force stream_prefetch $root/output/pcm_fix_20260907/stream_prefetch -part xc7z020clg484-2
add_files -norecurse $root/repo/tx_bit_engine/src/tx_bit_engine_axis.v
add_files -fileset sim_1 -norecurse $root/repo/tx_bit_engine/sim/tb_stream_prefetch.sv
set_property top tb_stream_prefetch [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
