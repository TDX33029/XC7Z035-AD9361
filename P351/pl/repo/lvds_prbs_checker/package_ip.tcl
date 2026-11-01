set root_dir [file normalize ./repo/lvds_prbs_checker]
set work_dir [file normalize ./tmp/lvds_prbs_checker_package]
create_project -force lvds_prbs_checker_package $work_dir -part xc7z020clg484-2
add_files -norecurse [glob $root_dir/src/*.v]
add_files -fileset constrs_1 -norecurse $root_dir/lvds_prbs_checker.xdc
set_property USED_IN {synthesis implementation} [get_files $root_dir/lvds_prbs_checker.xdc]
set_property top lvds_prbs_checker [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor user.org -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name lvds_prbs_checker $core
set_property display_name {AD9361 LVDS PN9 Checker} $core
set_property description {Self-synchronising four-channel PN9 checker with AXI-Lite status snapshot} $core
set_property version 1.0 $core
ipx::infer_bus_interfaces xilinx.com:interface:aximm_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::associate_bus_interfaces -busif S_AXI -clock S_AXI_ACLK $core
set dclk [ipx::get_bus_interfaces data_clk -of_objects $core]
if {[llength $dclk]} { set_property value rst [ipx::get_bus_parameters ASSOCIATED_RESET -of_objects $dclk] }
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
