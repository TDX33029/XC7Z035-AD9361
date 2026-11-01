file mkdir {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/tmpproj}
create_project tmpbd {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/tmpproj} -part xc7z035ffg900-2 -force
set_property platform.name {P351_sdboot} [current_project]
set_property platform.board_id {P351_sdboot} [current_project]
set_property ip_repo_paths {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/repo} [current_project]
update_ip_catalog -quiet
source {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/source_bd_sd.tcl}
generate_target all [get_files {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/tmpproj/tmpbd.srcs/sources_1/bd/design_1/design_1.bd}]
write_hw_platform -minimal -force -file {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/p351_sd.xsa}
puts "XSA written"
exit
