set root_dir [file normalize ./repo/tfs_spi]
create_project -force tfs_spi_package ./tmp/tfs_spi_package -part xc7z020clg484-2
add_files -norecurse $root_dir/tfs_spi.v
set_property top tfs_spi [current_fileset]
update_compile_order -fileset sources_1
ipx::package_project -root_dir $root_dir -vendor xilinx.com -library user -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name tfs_spi $core
set_property display_name {TFS Frequency Synthesizer SPI} $core
set_property version 1.0 $core
set_property core_revision 3 $core
ipx::infer_bus_interfaces xilinx.com:interface:aximm_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::associate_bus_interfaces -busif S_AXI -clock S_AXI_ACLK $core
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
