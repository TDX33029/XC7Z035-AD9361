open_project {D:/Document/tempPrj/XC7Z035-AD9361/P351/P351_AD9361TX/P351_AD9361TX.xpr}
set_property ip_repo_paths {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/repo} [current_project]
update_ip_catalog -quiet
set bdfile [get_files -quiet -of_objects [get_filesets sources_1] -filter {FILE_TYPE == Block Designs}]
if {![llength $bdfile]} { set bdfile [get_files -quiet design_1.bd] }
puts "BD=|$bdfile|"
open_bd_design [lindex $bdfile 0]
foreach ip [get_ips -quiet] {
    set st [get_property IP_STATUS $ip]
    if {[string match "*ock*" $st]} {
        set rs ""
        catch { set rs [get_property IP_LOCKED_REASONS $ip] }
        puts "LOCKED_IP|$ip|$st|$rs"
    }
}
exit
