set root_dir [file normalize ./repo/dds_inc]
set work_dir [file normalize ./tmp/dds_inc_package]
create_project -force dds_inc_package $work_dir -part xc7z020clg484-2
add_files -norecurse [list $root_dir/dds_inc.v $root_dir/pcm_fm_modulator.v \
    $root_dir/soqpsk_tg_modulator.v \
    $root_dir/irig_randomizer.v]
set_property top dds_inc [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $root_dir -vendor xilinx.com -library user \
    -taxonomy /UserIP -import_files -set_current true -force
set core [ipx::current_core]
set_property name dds_inc $core
set_property display_name {IRIG-106 waveform source and unified selector} $core
set_property description {Dual-channel complex sine/CW, PCM/FM and SOQPSK-TG source with a unified waveform selector} $core
set_property version 1.12 $core
set_property core_revision 1 $core
ipx::infer_bus_interfaces xilinx.com:signal:clock_rtl:1.0 $core
ipx::infer_bus_interfaces xilinx.com:signal:reset_rtl:1.0 $core
ipx::check_integrity -quiet $core
ipx::update_checksums $core
ipx::save_core $core
close_project
