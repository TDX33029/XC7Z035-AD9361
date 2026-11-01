create_project -in_memory -part xc7z035ffg900-2
set_property platform.name {P351} [current_project]
set_property platform.board_id {P351} [current_project]
set_property ip_repo_paths {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/repo} [current_project]
update_ip_catalog -quiet
source {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/source_bd_sd.tcl}
write_hw_platform -minimal -force -file {D:/Document/tempPrj/XC7Z035-AD9361/.auto/sdboot/p351_sd.xsa}
puts "XSA written"
exit
