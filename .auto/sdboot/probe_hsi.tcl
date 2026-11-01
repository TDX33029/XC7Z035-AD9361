lappend auto_path {C:/Xilinx/Vivado/2021.2/scripts/hsm}
package require hsi
puts "PROCS: [lsort [info procs hsi*]]"
puts "NS: [namespace children ::hsi]"
puts "CMDS: [lsort [info commands hsi*]]"
if {![catch {set h [hsi::open_hw_design {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/tmpproj/tmpbd.gen/sources_1/bd/design_1/hw_handoff/design_1.hwh}]} msg]} {
    puts "HWSW_OPEN_OK: $h"
} else {
    puts "OPEN_FAIL: $msg"
}
exit
