# Test source named by the generated project descriptor, not just repository.
set root [file normalize [file join [file dirname [info script]] ../../..]]
set descriptor [file join $root Z7sdrlite_noos_cmos.srcs sources_1 bd design_1 ip design_1_dds_inc_0_0 design_1_dds_inc_0_0.xml]
set handle [open $descriptor r]
set descriptor_text [read $handle]
close $handle
if {![regexp {<spirit:name>([^<]*dds_inc\.v)</spirit:name>} $descriptor_text match source_relative]} {
    error "Cannot resolve project DDS source"
}
set source_file [file normalize [file join [file dirname $descriptor] $source_relative]]
set work [file join $root tmp cw_project_source_r42]
puts "TESTING ACTUAL PROJECT SOURCE: $source_file"
create_project -force cw_project_source_r42 $work -part xc7z020clg484-2
add_files -norecurse $source_file
add_files -fileset sim_1 -norecurse $root/repo/dds_inc/sim/tb_cw_spectrum.sv
set_property top tb_cw_spectrum [get_filesets sim_1]
launch_simulation
run all
close_sim
close_project
