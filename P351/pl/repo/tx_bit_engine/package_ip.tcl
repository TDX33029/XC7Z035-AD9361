set root_dir [file normalize ./repo/tx_bit_engine]
set work_dir [file normalize ./tmp/tx_bit_engine_package]
create_project -force tx_bit_engine_package $work_dir -part xc7z020clg484-2
add_files -norecurse [glob $root_dir/src/*.v]
set_property top tx_bit_engine_v1_0 [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor user.org -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name tx_bit_engine $core
set_property display_name {TX BIT Engine} $core
set_property description {32-bit AXI4-Stream to exact-length MSB-first serial BIT engine with gapless PL replay buffer} $core
set_property version 1.2 $core
set_property core_revision 3 $core
set_property vendor_display_name {AD9361TX Project} $core
set_property company_url {} $core

ipx::infer_bus_interfaces xilinx.com:interface:aximm_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:interface:axis_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core

foreach busif [ipx::get_bus_interfaces -of_objects $core] {
    set bus_name [get_property name $busif]
    if {[string equal -nocase $bus_name "s00_axi"]} {
        set_property name S00_AXI $busif
    }
    if {[string equal -nocase $bus_name "s_axis"]} {
        set_property name S_AXIS $busif
    }
}

ipx::associate_bus_interfaces -busif S00_AXI -clock s00_axi_aclk $core
ipx::associate_bus_interfaces -busif S_AXIS -clock axis_clk $core
set axis_assoc [ipx::get_bus_parameters ASSOCIATED_BUSIF -of_objects \
    [ipx::get_bus_interfaces axis_clk -of_objects $core]]
set axi_assoc [ipx::get_bus_parameters ASSOCIATED_BUSIF -of_objects \
    [ipx::get_bus_interfaces s00_axi_aclk -of_objects $core]]
set_property value S_AXIS $axis_assoc
set_property value S00_AXI $axi_assoc
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
