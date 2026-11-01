create_project -in_memory -part xc7z035ffg900-2
set_property ip_repo_paths {D:/Document/tempPrj/XC7Z035-AD9361/P351/pl/repo} [current_project]
update_ip_catalog
foreach c [get_ipdefs -quiet *ad9361*] { puts "IPDEF: $c" }
exit
