lappend auto_path {C:/Xilinx/Vivado/2021.2/scripts/hsm}
package require hsi
hsi::set_repo_path -path {C:/Xilinx/Vivado/2021.2/data/embeddedsw}
set h [hsi::open_hw_design {D:/Document/tempPrj/XC7Z035-AD9361/P351/output/firmware/hardware.xsa}]
puts "HW: $h"
hsi::generate_bsp -proc ps7_cortexa9_0 -os standalone -dir {D:/Document/tempPrj/XC7Z035-AD9361/.auto/plbuild/app_bsp}
puts "BSP_DONE"
exit
