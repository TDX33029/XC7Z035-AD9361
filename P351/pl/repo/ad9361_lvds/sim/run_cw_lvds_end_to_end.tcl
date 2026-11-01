set root [file normalize .]
set work [file join $root tmp cw_lvds_end_to_end]
create_project -force cw_lvds_end_to_end $work -part xc7z020clg484-2
add_files -norecurse [list \
    [file join $root repo ad9361_lvds src ad9361_lvds_tx.v] \
    [file join $root repo dds_inc dds_inc.v] \
    [file join $root repo dds_inc pcm_fm_modulator.v] \
    [file join $root repo dds_inc soqpsk_tg_modulator.v] \
    [file join $root repo dds_inc irig_randomizer.v]]
add_files -fileset sim_1 -norecurse \
    [file join $root repo ad9361_lvds sim tb_cw_lvds_end_to_end.sv]
set_property top tb_cw_lvds_end_to_end [get_filesets sim_1]
launch_simulation
run all
close_sim
set sim_log [file join $work cw_lvds_end_to_end.sim sim_1 behav xsim simulate.log]
set log_file [open $sim_log r]
set log_text [read $log_file]
close $log_file
if {[string first "PASS CW through LVDS pins" $log_text] < 0 ||
    [string first "Fatal:" $log_text] >= 0} {
    error "CW LVDS end-to-end simulation failed; inspect $sim_log"
}
close_project
