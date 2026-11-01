set root [file normalize .]
set out [file join $root output tx_pl_audit_20260923]
file mkdir $out
open_checkpoint [file join $root Z7sdrlite_noos_cmos.runs impl_1 ad936x_top_routed.dcp]
set regs [get_cells -quiet -hier -filter {NAME =~ *tx_path/* && IS_SEQUENTIAL}]
if {[llength $regs]==0} {error "TX serializer not found"}
report_timing -to $regs -max_paths 30 -path_type full_clock_expanded -file [file join $out tx_internal_setup.rpt]
report_timing -to $regs -delay_type min -max_paths 20 -file [file join $out tx_internal_hold.rpt]
check_timing -verbose -file [file join $out check_timing.rpt]
report_clocks -file [file join $out clocks.rpt]
report_timing_summary -file [file join $out timing_summary.rpt]
close_design
