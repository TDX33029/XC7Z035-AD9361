set root_dir [file normalize ./repo/axi_lite_ctrl]
set work_dir [file normalize ./tmp/axi_lite_ctrl_package]
create_project -force axi_lite_ctrl_package $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/axi_lite_ctrl.v
set_property top axi_lite_ctrl [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor xilinx.com -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name axi_lite_ctrl $core
set_property display_name {AXI Lite AD9361 Control} $core
set_property description {AXI Lite control registers for AD9361 delay calibration, PCM/FM, SOQPSK-TG and ARTM CPM} $core
set_property version 1.3 $core
ipx::infer_bus_interfaces xilinx.com:interface:aximm_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::associate_bus_interfaces -busif S_AXI -clock S_AXI_ACLK $core
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
