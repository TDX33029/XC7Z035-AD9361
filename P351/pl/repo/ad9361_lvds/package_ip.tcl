set root_dir [file normalize ./repo/ad9361_lvds]
set work_dir [file normalize ./tmp/ad9361_lvds_package_lockfix_20260923]
create_project -force ad9361_lvds_package $work_dir -part xc7z020clg484-2
add_files -norecurse [glob $root_dir/src/*.v]
set_property top ad9361_lvds_phy [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor user.org -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name ad9361_lvds_phy $core
set_property display_name {AD9361 2R2T LVDS PHY} $core
set_property description {AD9361 2R2T LVDS PHY for 61.44 MSPS I/Q and 245.76 MHz interface clock} $core
set_property version 1.0 $core
set_property core_revision 6 $core
set_property vendor_display_name {AD9361TX Project} $core
set_property company_url {} $core

ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
