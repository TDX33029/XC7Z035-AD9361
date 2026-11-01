lappend auto_path {C:/Xilinx/Vivado/2021.2/scripts/hsm}
package require hsi
hsi open_hw_design {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/tmpproj/tmpbd.gen/sources_1/bd/design_1/hw_handoff/design_1.hwh}
hsi generate_app -app zynq_fsbl -proc ps7_cortexa9_0 -dir {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/fsbl_sd} -compile false
puts FSBL_GENERATED
exit
