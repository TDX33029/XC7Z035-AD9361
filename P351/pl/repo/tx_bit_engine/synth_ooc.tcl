create_project -force tx_bit_engine_ooc ./tmp/tx_bit_engine_ooc -part xc7z020clg484-2
add_files -norecurse [glob [file normalize ./repo/tx_bit_engine/src/*.v]]
set_property top tx_bit_engine_v1_0 [current_fileset]
update_compile_order -fileset sources_1
synth_design -top tx_bit_engine_v1_0 -part xc7z020clg484-2 -mode out_of_context
report_utilization -file [file normalize ./repo/tx_bit_engine/tx_bit_engine_ooc_utilization.rpt]
report_drc -file [file normalize ./repo/tx_bit_engine/tx_bit_engine_ooc_drc.rpt]
write_checkpoint -force [file normalize ./repo/tx_bit_engine/tx_bit_engine_ooc.dcp]
close_project
