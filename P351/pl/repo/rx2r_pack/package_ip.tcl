set root_dir [file normalize ./repo/rx2r_pack]
set work_dir [file normalize ./tmp/rx2r_pack_package]
create_project -force rx2r_pack_package $work_dir -part xc7z020clg484-2
add_files -norecurse $root_dir/src/rx2r_pack.v
set_property top rx2r_pack [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor xilinx.com -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name rx2r_pack $core
set_property display_name {TX DAC / RX ADC IQ Monitor Packer} $core
set_property description {Selects TX DAC or RX ADC dual-channel IQ and packs it into a 32-bit AXI stream without another DMA or FIFO} $core
set_property version 1.0 $core
set_property core_revision 3 $core
ipx::infer_bus_interfaces xilinx.com:interface:aximm_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:interface:axis_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::associate_bus_interfaces -busif S_AXI -clock S_AXI_ACLK $core
ipx::associate_bus_interfaces -busif m_axis -clock data_clk $core
set data_clk_if [ipx::get_bus_interfaces data_clk -of_objects $core]
set reset_param [ipx::get_bus_parameters -quiet ASSOCIATED_RESET -of_objects $data_clk_if]
if {[llength $reset_param] == 0} {
    set reset_param [ipx::add_bus_parameter ASSOCIATED_RESET $data_clk_if]
}
set_property value rst $reset_param
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
