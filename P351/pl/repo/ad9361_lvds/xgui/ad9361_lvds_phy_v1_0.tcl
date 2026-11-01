# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  ipgui::add_param $IPINST -name "Component_Name"
  #Adding Page
  set Page_0 [ipgui::add_page $IPINST -name "Page 0"]
  ipgui::add_param $IPINST -name "RX_ENABLE" -parent ${Page_0}


}

proc update_PARAM_VALUE.RX_ENABLE { PARAM_VALUE.RX_ENABLE } {
	# Procedure called to update RX_ENABLE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.RX_ENABLE { PARAM_VALUE.RX_ENABLE } {
	# Procedure called to validate RX_ENABLE
	return true
}


proc update_MODELPARAM_VALUE.RX_ENABLE { MODELPARAM_VALUE.RX_ENABLE PARAM_VALUE.RX_ENABLE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.RX_ENABLE}] ${MODELPARAM_VALUE.RX_ENABLE}
}

