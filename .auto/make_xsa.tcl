open_project {D:/Document/tempPrj/XC7Z035-AD9361/P351/P351_AD9361TX/P351_AD9361TX.xpr}
set_property ip_repo_paths {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/repo} [current_project]
update_ip_catalog -quiet
set bdfile [get_files -quiet -of_objects [get_filesets sources_1] -filter {FILE_TYPE == Block Designs}]
if {![llength $bdfile]} { set bdfile [get_files -quiet design_1.bd] }
open_bd_design [lindex $bdfile 0]
set locked [get_bd_cells -quiet -hierarchical -filter {LOCKED == 1}]
if {[llength $locked]} {
    puts "locked cells: $locked; attempting upgrade_ip"
    upgrade_ip -quiet [get_ips -quiet -filter {IS_LOCKED == 1}]
    close_bd_design [current_bd_design]
    open_bd_design [lindex $bdfile 0]
    set locked [get_bd_cells -quiet -hierarchical -filter {LOCKED == 1}]
    puts "still locked: $locked"
}
set ps7 [get_bd_cells processing_system7_0]

set_property CONFIG.PCW_EN_SDIO0 {1} $ps7
set_property CONFIG.PCW_SD0_PERIPHERAL_ENABLE {1} $ps7
set_property CONFIG.PCW_SDIO_PERIPHERAL_VALID {1} $ps7
set tree [get_property CONFIG.PCW_MIO_TREE_PERIPHERALS $ps7]
set toks [split $tree #]
puts "MIO tokens: [llength $toks]; 40..45 currently: [lrange $toks 40 45]"
for {set i 40} {$i < 46} {incr i} { lset toks $i {SD 0} }
set_property CONFIG.PCW_MIO_TREE_PERIPHERALS [join $toks #] $ps7

validate_bd_design
save_bd_design
generate_target all [get_files [lindex $bdfile 0]]

write_hw_platform -minimal -force -file {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/p351_sd.xsa}
puts "XSA written"
exit
