# DMA软件复位必须同时清除AXIS FIFO，防止STOP/超时后旧字混入下一任务。
# 只复位各自FIFO，保留系统复位来源，不复位AD9361/SPI/显示屏或另一路DMA。
proc p351_dma_fifo_reset {} {
    foreach {name fifo primary upstream} {
        dma_rx_fifo_reset axis_data_fifo_0 axi_dma_0/s2mm_prmry_reset_out_n util_vector_logic_1/Res
        dma_tx_fifo_reset axis_data_fifo_tx_0 axi_dma_tx_0/mm2s_prmry_reset_out_n proc_sys_reset_0/peripheral_aresetn
    } {
        if {![llength [get_bd_cells -quiet $name]]} {
            set gate [create_bd_cell -type ip -vlnv xilinx.com:ip:util_vector_logic:2.0 $name]
            set_property -dict [list CONFIG.C_OPERATION {and} CONFIG.C_SIZE {1}] $gate
            set pin [get_bd_pins $fifo/s_axis_aresetn]
            set old [get_bd_nets -of_objects $pin]
            if {[llength $old] != 1} {error "Missing existing FIFO reset: $fifo"}
            disconnect_bd_net $old $pin
            connect_bd_net [get_bd_pins $upstream] [get_bd_pins $name/Op1]
            connect_bd_net [get_bd_pins $primary] [get_bd_pins $name/Op2]
            connect_bd_net [get_bd_pins $name/Res] $pin
        }
        foreach {a b} [list $upstream $name/Op1 $primary $name/Op2 $name/Res $fifo/s_axis_aresetn] {
            if {[get_bd_nets -of_objects [get_bd_pins $a]] ne [get_bd_nets -of_objects [get_bd_pins $b]]} {
                error "DMA FIFO reset wiring mismatch: $a -> $b"
            }
        }
    }
    puts "P351_DMA_FIFO_RESET_CONNECTED: MM2S/TX FIFO and S2MM/RX FIFO, independent soft resets"
}
p351_dma_fifo_reset
