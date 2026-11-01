# The checker accepts a decoded 1R1T word no more often than once per two
# 122.88 MHz DATA_CLK cycles.  accept_valid enforces that contract even if a
# malformed FRAME produces early adc_valid pulses.
set pn_state_regs [filter [get_cells -quiet -hier -regexp {.*core/c_[iq][01]/.*_reg.*}] {IS_SEQUENTIAL}]
set pn_count_regs [filter [get_cells -quiet -hier -regexp {.*core/(err_[iq][01]|oos_count)_reg.*}] {IS_SEQUENTIAL}]
set pn_sample_regs [filter [get_cells -quiet -hier -regexp {.*core/sample_[iq][01]_reg.*}] {IS_SEQUENTIAL}]
set pn_ctrl_regs [filter [get_cells -quiet -hier -regexp {.*(en_s2|clr_s2|clr_d|enable_d|checker_valid|all_locked_d)_reg.*}] {IS_SEQUENTIAL}]
set_multicycle_path 2 -setup -from $pn_state_regs -to $pn_state_regs
set_multicycle_path 1 -hold  -from $pn_state_regs -to $pn_state_regs
set_multicycle_path 2 -setup -from $pn_state_regs -to $pn_count_regs
set_multicycle_path 1 -hold  -from $pn_state_regs -to $pn_count_regs
set_multicycle_path 2 -setup -from $pn_sample_regs -to $pn_state_regs
set_multicycle_path 1 -hold  -from $pn_sample_regs -to $pn_state_regs
set_multicycle_path 2 -setup -from $pn_sample_regs -to $pn_count_regs
set_multicycle_path 1 -hold  -from $pn_sample_regs -to $pn_count_regs
set_multicycle_path 2 -setup -from $pn_count_regs -to $pn_count_regs
set_multicycle_path 1 -hold  -from $pn_count_regs -to $pn_count_regs
# AXI-derived enable/clear controls are synchronised and remain stable for the
# complete measurement window.  Their effect only has to settle before the next
# accepted decoded word, which is two DATA_CLK cycles later.
set_multicycle_path 2 -setup -from $pn_ctrl_regs -to $pn_state_regs
set_multicycle_path 1 -hold  -from $pn_ctrl_regs -to $pn_state_regs
set_multicycle_path 2 -setup -from $pn_ctrl_regs -to $pn_count_regs
set_multicycle_path 1 -hold  -from $pn_ctrl_regs -to $pn_count_regs
