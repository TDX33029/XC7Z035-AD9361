lappend auto_path {C:/Xilinx/Vivado/2021.2/scripts/hsm}
package require hsi
puts "INTERNAL_PROCS: [lsort [info procs ::hsi::internal::*]]"
puts "INTERNAL_CMDS: [lsort [info commands ::hsi::*]]"
exit
