set root [file normalize .]
create_project -force tx_pin_integrity [file join $root tmp tx_pin_integrity] -part xc7z020clg484-2
add_files [file join $root repo ad9361_lvds src ad9361_lvds_tx.v]
add_files -fileset sim_1 [file join $root repo ad9361_lvds sim tb_tx_pin_integrity.sv]
set_property top tb_tx_pin_integrity [get_filesets sim_1]
if {[info exists ::env(TX_CDC_STRESS)]} {
  set_property xsim.simulate.xsim.more_options {-testplusarg CDC_STRESS} [get_filesets sim_1]
}
launch_simulation
run all
close_sim
close_project
