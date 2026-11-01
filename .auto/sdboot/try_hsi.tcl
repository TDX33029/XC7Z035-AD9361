lappend auto_path {C:/Xilinx/Vivado/2021.2/scripts/hsm}
if {[catch {package require hsi} msg]} { puts "HSI_FAIL: $msg" } else { puts "HSI_OK: $msg" }
exit
