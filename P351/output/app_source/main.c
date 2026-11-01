/***************************************************************************//**
 *   @file   main.c
 *   @brief  AD9361 1R1T 初始化、调制控制、I/Q采集及上电参数管理
 *   @date   2026-10-04
 *
 *  硬件：仅物理TX2/RX2，I/Q采样率61.44 MSPS，LVDS DATA_CLK 122.88 MHz DDR。
 *  PS负责参数校验、AD9361配置、PN11组帧、DMA、网口与屏幕；PL负责实时调制和LVDS。
 *
 *  上电参数只改 boot_pcm_fm_profile.h；修改后必须重新编译/固化，不是在线改寄存器。
 *  依赖匹配的XSA/BSP与xparameters.h；Flash启动需QSPI。UI读取CFGQ不会改变发射。
 *******************************************************************************/

#include "config.h"
#include "ad9361_api.h"
#include "parameters.h"
#include "rf_channel_profile.h"
#include "platform.h"
#include "dma_rx.h"
#include "eth_link.h"
#include "udp_stack.h"
#include "app_upload.h"
#include "dma_tx_bit.h"
#include "tx_bit_protocol.h"
#include "lvds_calibration.h"
#include "screen_uart0.h"
#include <xil_cache.h>
#include "xil_assert.h"
#include "xil_types.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "sleep.h"
#include "boot_pcm_fm_profile.h"
#include "p351_board.h"
#include "boot_diagnostics.h"
#include "board_config.h"
#include "p351_clock.h"
#include "p351_clock_profile.h"
int board_final_rf_owned(void);
#include "rx_receiver.h"
#include "tfs_control.h"
#include "p351_tx_system.h"
#if RF_RX_PHYSICAL_CHANNEL != 2U || RF_TX_PHYSICAL_CHANNEL != 2U
#error "P351 board-switch profile currently supports physical RX2/TX2 only"
#endif
#include "xtime_l.h"
#include "xuartps_hw.h"
#include <stdio.h>

/* ==================== 运行时射频缓存：1R1T / 61.44 MSPS ==================== */
uint32_t sample_rate = 61440000UL; /* 61.44 MSPS; LVDS 1R1T DATA_CLK=2*Fs=122.88 MHz */
uint64_t tx_lo_freq = BOOT_INITIAL_TX_LO_HZ; /* 人工UI版初始2.4GHz；屏幕方案独立使用70MHz；RF默认关闭。 */
uint64_t rx_lo_freq = 4000e6; /* 默认RX LO 4 GHz；外接SMA自收模式由受保护流程跟随TX LO。 */
uint32_t bandwidth = BOOT_PCM_FM_BW_HZ; /* TX固定56MHz模拟带宽，不等于调制B99。 */
static uint32_t rx_bandwidth_current = 20e6;
static u64 rx_lo_applied_hz = 4000000000ULL;
static u32 rx_bw_applied_hz = 20000000UL;
int32_t gain_rx1 = 45; /* 逻辑槽0的增益，实际用于RX2；SMA自收仍需外部衰减保护。 */
int32_t gain_rx2 = 45; /* 兼容旧接口的空槽参数，不调用第二逻辑通道API。 */

/* TX单音诊断：实际2 MHz复数单音由PL DDS生成。
 * 下列旧相位常量仅兼容历史代码，不是当前上电PCM/FM参数入口。 */
#define TX_TONE_PINC   0x0CCCCCCC

/* TX衰减单位mdB；89.75 dB用于故障时第二层保护。
 * 正常停发必须关闭TX通道和TX LO，不能仅用大衰减冒充停发。 */
#define TX_MUTE_ATTEN_MDB 89750UL
/* Provisional CW linearity guard, NOT an AD9361 guaranteed SFDR limit.
 * Keep digital DAC resolution; back off the RF output stage instead.
 * Requalify this minimum with RF measurements before allowing higher power. */
#if P351_MANUAL_RF_TEST
#define TX_CW_MIN_ATTEN_MDB 0UL /* 人工直发不再偷偷将CW的衰减抬到10dB。 */
#else
#define TX_CW_MIN_ATTEN_MDB 10000UL
#endif
/* tx1_* / tx2_* 是历史协议槽位名，不是物理端口名。
 * 槽0(tx1_*)控制物理TX2，槽1(tx2_*)禁用；PL保持第一组I/Q。 */
#if P351_MANUAL_RF_TEST
static u32 tx1_atten_mdB = TX_MUTE_ATTEN_MDB;
#else
static u32 tx1_atten_mdB = 10000;
#endif
static u32 tx2_atten_mdB = 10000;
static u8 tx1_mode_current = 0;
static u8 tx2_mode_current = 0;
static u8 tx_bist_active = 0;
static XTime tx_bist_deadline = 0;
static XTime tx_bist_retry_at = 0;
static u8 tx_bist_disable_failures = 0;
static u32 pcm_deviation_word_current = 0;
static u32 pcm_bit_rate_word_current = 0;
static u32 pcm_frequency_offset_word_current = 0;
static u16 pcm_amplitude_current = 0;
static u8 pcm_filter_profile_current = 0;
static u8 pcm_randomizer_current = 0;
static u8 pcm_enable_current = 0;
static u8 line_code_current = 0;
static u8 rolloff_current = 0;
static u8 refclk_source_current = 0;
static u32 refclk_rate_current = 40000000UL;
/* 内部40MHz仅用于芯片初始校准；未完成上电默认参考事务前不能宣称有效。 */
static u8 refclk_valid = 0U;
static u8 boot_reference_pending;
/* 换频失败后禁止仅用模式命令重新开RF；须由完整TXCF事务重新验证。 */
static u8 tx_rf_config_inhibited = 0U;
static u8 tx_rf_retune_failed = 0U;
static u32 soqpsk_bit_rate_word_current = 0;
static u32 soqpsk_frequency_offset_word_current = 0;
static u16 soqpsk_amplitude_current = 0;
static u8 soqpsk_differential_current = 1;
static u8 soqpsk_enable_current = 0;
static u32 boot_pcm_fm_result = BOOT_PCM_FM_ENABLED ? 1U : 0U;
/* 屏幕本地任务使用独立ID空间；网络接管后不得停止网络的新任务。 */
static u32 screen_task_id;
static u8 screen_task_active, screen_task_infinite;
static lvds_cal_result_t lvds_cal_result;

/* AD9361数字回环绕过RF/LO，不可用作外接SMA收发成功的证据。 */
static u8 tx_rx_loopback_current = 0;
static u8 rx_rf_monitor_current = 0;
#define RX_RF_MONITOR_GAIN_DB 10

/* 旧RX BIST常量：Fs/16=3.84 MHz；TX BIST诊断有独立配置与自动超时。 */
#define BIST_TONE_HZ    3840000UL

/* V6：131072个真实复样本、每组一个32bit字，约2.133ms，足够4M默认
 * 小帧的相邻两次同步字。UDP再补零槽兼容旧绘图；不可拼接有间隙的SNAP。 */
u32 rx_buf[UDP_CAPTURE_WORDS] __attribute__((aligned(64)));

/* AD9361初始化参数：1R1T，收发采样率固定61.44 MSPS。
 * 不要直接修改长初始化表的TX LO；上电方案通过BOOT_INITIAL_TX_LO_HZ集中设置。 */
struct ad9361_rf_phy *ad9361_phy;

s32 board_temperature_mC(void) {
	static XTime last;
	static s32 cached = (-2147483647 - 1);
	static u8 sampled;
	XTime now;
	int32_t raw, valid;
	XTime_GetTime(&now);
	if (sampled && now - last < COUNTS_PER_SECOND)
		return cached;
	last = now;
	sampled = 1;
	cached = (-2147483647 - 1);
	if (!ad9361_phy)
		return cached;
	/* Periodic measurement is configured at init. Read only: no calibration,
	 * ENSM changes, AUXADC power toggling or waiting in the UDP service loop. */
	valid = ad9361_spi_read(ad9361_phy->spi, REG_START_TEMP_READING);
	if (valid < 0 || !(valid & 2))
		return cached;
	raw = ad9361_spi_read(ad9361_phy->spi, REG_TEMPERATURE);
	if (raw <= 0 || raw >= 255)
		return cached; /* reject saturation/unavailable */
	/* Same scaling as the bundled ADI driver, factory offset configured at init.
	 * This is an uncalibrated sensor estimate, not ambient/case temperature. */
	cached = (raw * 1000000 + 570) / 1140;
	return cached;
}

/* Last SPI-verified RX settings; safe to query from the UDP receive path. */
u32 board_rx_lo_khz(void) { return (u32)(rx_lo_applied_hz / 1000ULL); }
u32 board_rx_bw_hz(void) { return rx_bw_applied_hz; }
u32 board_rx_status(void)
{
	return (rx_rf_monitor_current ? 1U : 0U) |
			(lvds_cal_result.valid ? 2U : 0U) |
			(rx2r_get_source() == RX2R_SOURCE_RX_ADC ? 4U : 0U) |
			((rx2r_status() & RX2R_ST_SOURCE_TX) == 0U ? 8U : 0U);
}

/* 返回当前已应用参数，不把“上电目标功率”当成射频实测值；整包由主循环一次生成。 */
void board_config_snapshot(u32 words[BOARD_CONFIG_WORDS])
{
	u8 soq = tx1_mode_current == TX_MODE_SOQPSK_TG;
	memset(words, 0, BOARD_CONFIG_WORDS * sizeof(*words));
	words[BC_VERSION] = BOARD_CONFIG_VERSION;
	words[BC_FLAGS] = (refclk_valid ? 1U : 0U) | (BOOT_PCM_FM_ENABLED ? 2U : 0U) |
			BOARD_CONFIG_P351 | (P351_RF_HARDWARE_QUALIFIED ? BOARD_CONFIG_HW_QUALIFIED : 0U) |
			(p351_rx_iq_capable() ? BOARD_CONFIG_RX_IQ_FIXED : 0U) |
			(p351_single_slot_capable() ? BOARD_CONFIG_SINGLE_SLOT : 0U) |
			(p351_rx_dsp_capable() ? BOARD_CONFIG_RX_DSP : 0U) |
			(p351_rx_timing_capable() ? BOARD_CONFIG_RX_TIMING : 0U) |
			(pcm_enable_current ? 4U : 0U) | (soqpsk_enable_current ? 8U : 0U) |
			(tx_bist_active ? 128U : 0U) |
			(RF_RX_PHYSICAL_CHANNEL == 2U ? 256U : 0U) |
			(RF_TX_PHYSICAL_CHANNEL == 2U ? 512U : 0U) |
			(refclk_valid && !tx_rf_config_inhibited && !tx_rf_retune_failed ?
					BOARD_CONFIG_RF_VALID : 0U);
	words[BC_TX1] = tx1_mode_current;
	words[BC_TX2] = tx2_mode_current;
	words[BC_LO_LOW] = (u32)tx_lo_freq;
	words[BC_LO_HIGH] = (u32)(tx_lo_freq >> 32);
	words[BC_BW] = bandwidth;
	words[BC_FS] = sample_rate;
	words[BC_ATT1] = tx1_atten_mdB;
	words[BC_ATT2] = tx2_atten_mdB;
	words[BC_RATE_WORD] = soq ? soqpsk_bit_rate_word_current : pcm_bit_rate_word_current;
	words[BC_DEV_WORD] = pcm_deviation_word_current;
	words[BC_OFFSET_WORD] = soq ? soqpsk_frequency_offset_word_current : pcm_frequency_offset_word_current;
	words[BC_AMPLITUDE] = soq ? soqpsk_amplitude_current : pcm_amplitude_current;
	words[BC_LINE] = line_code_current;
	words[BC_ROLLOFF] = rolloff_current;
	words[BC_RANDOMIZER] = soq ? (soqpsk_enable_current ? 1U : 0U) : pcm_randomizer_current;
	words[BC_DIFFERENTIAL] = soqpsk_differential_current;
	words[BC_REF_SOURCE] = refclk_source_current;
	words[BC_REF_RATE] = refclk_rate_current;
	words[BC_ZERO_MDBM] = (u32)BOOT_PCM_FM_ZERO_MDBM;
	words[BC_TARGET_MDBM] = (u32)BOOT_PCM_FM_TARGET_MDBM;
	words[BC_BOOT_RESULT] = boot_pcm_fm_result;
	tx_bit_protocol_snapshot(words);
	if (!p351_clock_ready()) words[BC_FLAGS] &= ~(1U | BOARD_CONFIG_RF_VALID);
}

AD9361_InitParam default_init_param = { ID_AD9361,	// dev_sel
		0,		// id_no
		40000000UL,	// reference_clk_rate
		/* Base Configuration */
		0,		//two_rx_two_tx_mode_enable：保持1R1T
		RF_RX_PHYSICAL_CHANNEL, //one_rx_one_tx_mode_use_rx_num：物理RX2
		RF_TX_PHYSICAL_CHANNEL, //one_rx_one_tx_mode_use_tx_num：物理TX2
		1,		//frequency_division_duplex_mode_enable
		0,		//frequency_division_duplex_independent_mode_enable
		0,		//tdd_use_dual_synth_mode_enable
		0,		//tdd_skip_vco_cal_enable
		0,		//tx_fastlock_delay_ns
		0,		//rx_fastlock_delay_ns
		0,		//rx_fastlock_pincontrol_enable
		0,		//tx_fastlock_pincontrol_enable
		0,		//external_rx_lo_enable
		0,		//external_tx_lo_enable
		5,		//dc_offset_tracking_update_event_mask
		6,		//dc_offset_attenuation_high_range
		5,		//dc_offset_attenuation_low_range
		0x28,	//dc_offset_count_high_range
		0x32,	//dc_offset_count_low_range
		0,		//split_gain_table_mode_enable
		MAX_SYNTH_FREF,	//trx_synthesizer_target_fref_overwrite_hz
		0,		//qec_tracking_slow_mode_enable
		/* ENSM Control */
		0,		//ensm_enable_pin_pulse_mode_enable
		0,		//ensm_enable_txnrx_control_enable
		/* LO Control */
		4000000000UL,//rx_synthesizer_frequency_hz：初始化RX LO为4 GHz
		BOOT_INITIAL_TX_LO_HZ,	//TX初始化中心70MHz；其余变频由外部组件负责
		1,		//tx_lo_powerdown_managed_enable
		/* Rate & BW Control */
		{ 983040000, 245760000, 122880000, 61440000, 61440000, 61440000 },//rx_path_clock_frequencies
		{ 983040000, 122880000, 122880000, 61440000, 61440000, 61440000 },//tx_path_clock_frequencies
		20000000,	//rf_rx_bandwidth_hz
		BOOT_PCM_FM_BW_HZ,	//TX模拟带宽；高码率不能强制沿用20MHz
		/* RF Port Control */
		0,		//rx_rf_port_input_select
		0,		//tx_rf_port_input_select
		/* TX Attenuation Control */
		89750,	// Applied after initial TX QEC in ad9361_setup; not a calibration-time RF mute.
		0,		//update_tx_gain_in_alert_enable
		/* Reference Clock Control */
		1,		// P351: 40 MHz缓冲VCTCXO输入，按厂家DTB禁用AD9361自身XO电路。
		{ 8, 5920 },	//dcxo_coarse_and_fine_tune
		0,		//clk_output_mode_select
		/* Gain Control */
		0,		//gc_rx1_mode
		0,		//gc_rx2_mode
		58,		//gc_adc_large_overload_thresh
		4,		//gc_adc_ovr_sample_size
		47,		//gc_adc_small_overload_thresh
		8192,	//gc_dec_pow_measurement_duration
		0,		//gc_dig_gain_enable
		800,	//gc_lmt_overload_high_thresh
		704,	//gc_lmt_overload_low_thresh
		24,		//gc_low_power_thresh
		15,		//gc_max_dig_gain
		/* Gain MGC Control */
		2,		//mgc_dec_gain_step
		2,		//mgc_inc_gain_step
		0,		//mgc_rx1_ctrl_inp_enable
		0,		//mgc_rx2_ctrl_inp_enable
		0,		//mgc_split_table_ctrl_inp_gain_mode
		/* Gain AGC Control */
		10,		//agc_adc_large_overload_exceed_counter
		2,		//agc_adc_large_overload_inc_steps
		0,		//agc_adc_lmt_small_overload_prevent_gain_inc_enable
		10,		//agc_adc_small_overload_exceed_counter
		4,		//agc_dig_gain_step_size
		3,		//agc_dig_saturation_exceed_counter
		1000,	//agc_gain_update_interval_us
		0,		//agc_immed_gain_change_if_large_adc_overload_enable
		0,		//agc_immed_gain_change_if_large_lmt_overload_enable
		10,		//agc_inner_thresh_high
		1,		//agc_inner_thresh_high_dec_steps
		12,		//agc_inner_thresh_low
		1,		//agc_inner_thresh_low_inc_steps
		10,		//agc_lmt_overload_large_exceed_counter
		2,		//agc_lmt_overload_large_inc_steps
		10,		//agc_lmt_overload_small_exceed_counter
		5,		//agc_outer_thresh_high
		2,		//agc_outer_thresh_high_dec_steps
		18,		//agc_outer_thresh_low
		2,		//agc_outer_thresh_low_inc_steps
		1,		//agc_attack_delay_extra_margin_us
		0,		//agc_sync_for_gain_counter_enable
		/* Fast AGC */
		64,		//fagc_dec_pow_measuremnt_duration
		260,	//fagc_state_wait_time_ns
		/* Fast AGC - Low Power */
		0,		//fagc_allow_agc_gain_increase
		5,		//fagc_lp_thresh_increment_time
		1,		//fagc_lp_thresh_increment_steps
		/* Fast AGC - Lock Level */
		1,		//fagc_lock_level_lmt_gain_increase_en
		5,		//fagc_lock_level_gain_increase_upper_limit
		/* Fast AGC - Peak Detectors and Final Settling */
		1,		//fagc_lpf_final_settling_steps
		1,		//fagc_lmt_final_settling_steps
		3,		//fagc_final_overrange_count
		/* Fast AGC - Final Power Test */
		0,		//fagc_gain_increase_after_gain_lock_en
		/* Fast AGC - Unlocking the Gain */
		0,		//fagc_gain_index_type_after_exit_rx_mode
		1,		//fagc_use_last_lock_level_for_set_gain_en
		1,		//fagc_rst_gla_stronger_sig_thresh_exceeded_en
		5,		//fagc_optimized_gain_offset
		10,		//fagc_rst_gla_stronger_sig_thresh_above_ll
		1,		//fagc_rst_gla_engergy_lost_sig_thresh_exceeded_en
		1,		//fagc_rst_gla_engergy_lost_goto_optim_gain_en
		10,		//fagc_rst_gla_engergy_lost_sig_thresh_below_ll
		8,		//fagc_energy_lost_stronger_sig_gain_lock_exit_cnt
		1,		//fagc_rst_gla_large_adc_overload_en
		1,		//fagc_rst_gla_large_lmt_overload_en
		0,		//fagc_rst_gla_en_agc_pulled_high_en
		0,		//fagc_rst_gla_if_en_agc_pulled_high_mode
		64,		//fagc_power_measurement_duration_in_state5
		/* RSSI Control */
		1,		//rssi_delay
		1000,	//rssi_duration
		3,		//rssi_restart_mode
		0,		//rssi_unit_is_rx_samples_enable
		1,		//rssi_wait
		/* Aux ADC Control */
		256,	//aux_adc_decimation
		40000000UL,	//aux_adc_rate
		/* AuxDAC Control */
		1,		//aux_dac_manual_mode_enable
		0,		//aux_dac1_default_value_mV
		0,		//aux_dac1_active_in_rx_enable
		0,		//aux_dac1_active_in_tx_enable
		0,		//aux_dac1_active_in_alert_enable
		0,		//aux_dac1_rx_delay_us
		0,		//aux_dac1_tx_delay_us
		0,		//aux_dac2_default_value_mV
		0,		//aux_dac2_active_in_rx_enable
		0,		//aux_dac2_active_in_tx_enable
		0,		//aux_dac2_active_in_alert_enable
		0,		//aux_dac2_rx_delay_us
		0,		//aux_dac2_tx_delay_us
		/* Temperature Sensor Control */
		256,	//temp_sense_decimation
		1000,	//temp_sense_measurement_interval_ms
		0xCE,	//temp_sense_offset_signed
		1,		//temp_sense_periodic_measurement_enable
		/* Control Out Setup */
		0xFF,	//ctrl_outs_enable_mask
		0,		//ctrl_outs_index
		/* External LNA Control */
		0,		//elna_settling_delay_ns
		0,		//elna_gain_mdB
		0,		//elna_bypass_loss_mdB
		0,		//elna_rx1_gpo0_control_enable
		0,		//elna_rx2_gpo1_control_enable
		0,		//elna_gaintable_all_index_enable
		/* Digital Interface Control */
		0,		//digital_interface_tune_skip_mode
		1,		//digital_interface_tune_fir_disable
		1,		//pp_tx_swap_enable
		1,		//pp_rx_swap_enable
		0,		//tx_channel_swap_enable
		0,		//rx_channel_swap_enable
		1,		//rx_frame_pulse_mode_enable
		0,		//two_t_two_r_timing_enable
		0,		//invert_data_bus_enable
		0,		//invert_data_clk_enable
		0,		//fdd_alt_word_order_enable
		0,		//invert_rx_frame_enable
		0,		//fdd_rx_rate_2tx_enable
		0,		//swap_ports_enable (unsupported in LVDS)
		0,		//single_data_rate_enable
		1,		//lvds_mode_enable
		0,		//half_duplex_mode_enable
		0,		//single_port_mode_enable
		0,		//full_port_enable (unsupported in LVDS)
		0,		//full_duplex_swap_bits_enable
		0,		//delay_rx_data
		0,		//rx_data_clock_delay
		4,		//rx_data_delay
		7,		//tx_fb_clock_delay
		0,		//tx_data_delay
		150,	//lvds_bias_mV
		1,		//lvds_rx_onchip_termination_enable
		0,		//rx1rx2_phase_inversion_en
        0xFF,	// lvds_invert1_control：按厂家/ADI设置，抵消芯片默认数据位反相。
        0x0F,	// lvds_invert2_control：时钟/FRAME不改变；0/0不是“无数据反相”。
		/* GPO Control */
		0,		//gpo0_inactive_state_high_enable
		0,		//gpo1_inactive_state_high_enable
		0,		//gpo2_inactive_state_high_enable
		0,		//gpo3_inactive_state_high_enable
		0,		//gpo0_slave_rx_enable
		0,		//gpo0_slave_tx_enable
		0,		//gpo1_slave_rx_enable
		0,		//gpo1_slave_tx_enable
		0,		//gpo2_slave_rx_enable
		0,		//gpo2_slave_tx_enable
		0,		//gpo3_slave_rx_enable
		0,		//gpo3_slave_tx_enable
		0,		//gpo0_rx_delay_us
		0,		//gpo0_tx_delay_us
		0,		//gpo1_rx_delay_us
		0,		//gpo1_tx_delay_us
		0,		//gpo2_rx_delay_us
		0,		//gpo2_tx_delay_us
		0,		//gpo3_rx_delay_us
		0,		//gpo3_tx_delay_us
		/* Tx Monitor Control */
		37000,	//low_high_gain_threshold_mdB
		0,		//low_gain_dB
		24,		//high_gain_dB
		0,		//tx_mon_track_en
		0,		//one_shot_mode_en
		511,	//tx_mon_delay
		8192,	//tx_mon_duration
		2,		//tx1_mon_front_end_gain
		2,		//tx2_mon_front_end_gain
		48,		//tx1_mon_lo_cm
		48,		//tx2_mon_lo_cm
		/* GPIO definitions */
		-1,		//gpio_resetb：main中替换为GPIO_RESET_PIN
		/* MCS Sync */
		-1,		//gpio_sync
		-1,		//gpio_cal_sw1
		-1,		//gpio_cal_sw2
		/* External LO clocks */
		NULL,	//(*ad9361_rfpll_ext_recalc_rate)()
		NULL,	//(*ad9361_rfpll_ext_round_rate)()
		NULL	//(*ad9361_rfpll_ext_set_rate)()
		};

AD9361_RXFIRConfig rx_fir_config = { 3, // rx;
		-6, // rx_gain;
		2, // rx_dec;
		{ -15 - 79, -116, -137, -119, -59, 27, 101, 127, 89, 4, -77, -102, -48,
				58, 151, 165, 78, -64, -170, -161, -27, 156, 266, 216, 18, -210,
				-312, -199, 80, 350, 420, 213, -163, -466, -471, -135, 352, 664,
				558, 49, -559, -847, -565, 172, 909, 1118, 564, -485, -1363,
				-1405, -417, 1101, 2163, 1879, 91, -2340, -3829, -2858, 1132,
				7263, 13459, 17341, 17341, 13459, 7263, 1132, -2858, -3829,
				-2340, 91, 1879, 2163, 1101, -417, -1405, -1363, -485, 564,
				1118, 909, 172, -565, -847, -559, 49, 558, 664, 352, -135, -471,
				-466, -163, 213, 420, 350, 80, -199, -312, -210, 18, 216, 266,
				156, -27, -161, -170, -64, 78, 165, 151, 58, -48, -102, -77, 4,
				89, 127, 101, 27, -59, -119, -137, -116, -79, -39, -15 }, // rx_coef[128]
		128, // rx_coef_size
		{ 0, 0, 0, 0, 0, 0 }, //rx_path_clks[6]
		0 // rx_bandwidth
		};

AD9361_TXFIRConfig tx_fir_config = { 3, // tx;
		0, // tx_gain;
		2, // tx_inc;
		{ -23, -44, -92, -124, -144, -114, -47, 46, 119, 141, 93, 4, -79, -98,
				-37, 73, 164, 170, 75, -69, -171, -154, -13, 171, 275, 215, 9,
				-218, -311, -187, 97, 363, 421, 203, -177, -471, -464, -118,
				368, 670, 550, 33, -571, -846, -551, 190, 918, 1114, 550, -498,
				-1364, -1394, -402, 1107, 2154, 1863, 84, -2324, -3793, -2826,
				1125, 7188, 13311, 17146, 17146, 13311, 7188, 1125, -2826,
				-3793, -2324, 84, 1863, 2154, 1107, -402, -1394, -1364, -498,
				550, 1114, 918, 190, -551, -846, -571, 33, 550, 670, 368, -118,
				-464, -471, -177, 203, 421, 363, 97, -187, -311, -218, 9, 215,
				275, 171, -13, -154, -171, -69, 75, 170, 164, 73, -37, -98, -79,
				4, 93, 141, 119, 46, -47, -114, -144, -124, -92, -44, -23 }, // tx_coef[128]
		128, // tx_coef_size
		{ 0, 0, 0, 0, 0, 0 }, // tx_path_clks[6]
		0 // tx_bandwidth
		};

/* ==================== 函数声明 ==================== */
void Gpio_Emio_Init(void);
void Axi_Ctrl_Write(u32 offset, u32 data);
static u8 Axi_Ctrl_Write_Checked(u32 offset, u32 data);
static void udp_net_init(void);
static void tx_apply_modes(u8 tx1_mode, u8 tx2_mode);
static void modulation_config_fail_closed(void);
static void tx_apply_config(u8 tx1_mode, u8 tx2_mode, u64 lo_hz, u32 bw_hz,
		u32 atten1_mdB, u32 atten2_mdB, u8 line_code, u8 rolloff);
static u32 refclk_apply_config(u8 source, u32 rate_hz);
static void refclk_transaction_service(void);
static int tx_lvds_port_verify(u8 repair);
static void pcm_apply_config(u32 bit_rate_word, u32 deviation_word,
		u32 frequency_offset_word, u16 amplitude, u8 filter_profile,
		u8 randomizer, u8 enable);
static void soqpsk_apply_config(u32 bit_rate_word, u32 frequency_offset_word,
		u16 amplitude, u8 differential, u8 randomizer, u8 enable);

static u8 tx_mode_is_bit(u8 mode) {
	return (mode == TX_MODE_SOQPSK_TG || mode == TX_MODE_PCM_FM) ? 1U : 0U;
}

static u8 tx_mode_valid(u8 mode) {
	return (mode == TX_MODE_OFF || mode == TX_MODE_SOQPSK_TG || mode == TX_MODE_PCM_FM
			|| mode == TX_MODE_CW) ? 1U : 0U;
}

static u8 tx_modes_mixed(u8 tx1_mode, u8 tx2_mode) {
	return (tx_mode_is_bit(tx1_mode) && tx_mode_is_bit(tx2_mode)
			&& tx1_mode != tx2_mode) ? 1U : 0U;
}

static u32 shape_control_word(void) {
	return (((u32) (line_code_current & 0x7U)) << PCM_CTRL_LINE_SHIFT)
			| (((u32) (rolloff_current & 0x7U)) << PCM_CTRL_ROLLOFF_SHIFT);
}

/* BSP断言诊断：打印文件/行号，关闭默认无限等待；不代表错误可以忽略。 */
static void dbg_assert_cb(const char8 *file, s32 line) {
	xil_printf("[assert] %s:%d\r\n", (const char *) file, (int) line);
	Xil_AssertWait = 0; /* 保留诊断输出，不在断言回调中永久阻塞。 */
}

/* Real RF gate.  Per-channel enable bits remove the selected TX datapath.
 *
 * This design configures AD9361 for FDD.  An FDD device cannot enter the TDD
 * RX-only ENSM state: the no-OS driver rejects that transition and leaves the
 * chip in ALERT (state 5).  Requiring RX (state 8) therefore made every OFF
 * transaction fail and aborted the following waveform configuration.  Keep
 * ENSM in FDD, gate the actual TX channel bits and explicitly power down the
 * TX LO; RX remains alive for the UDP monitor while RF TX is disabled. */
static int tx_rf_gate_apply(u8 tx1_enable, u8 tx2_enable) {
	int32_t status1, status2, ensm_status, lo_status, reg, lo_reg;
	int32_t bist_status;
	/* 参数是逻辑槽位；门控寄存器却使用物理通道掩码。 */
	u8 expected_channels = tx1_enable ? RF_TX_PHYSICAL_CHANNEL : 0U;
	u8 fdd =
			(ad9361_phy != NULL && ad9361_phy->pdata != NULL
					&& ad9361_phy->pdata->fdd) ? 1U : 0U;
	u8 expected_state =
			fdd ? ENSM_STATE_FDD : (
					expected_channels ? ENSM_STATE_TX : ENSM_STATE_RX);
	u8 state;
#if P351_MANUAL_RF_TEST
	if(expected_channels) {
		u32 actual_atten=0U;
		if(ad9361_phy==NULL ||
		   tx1_atten_mdB-P351_MANUAL_RF_MIN_ATTEN_MDB>89750U-P351_MANUAL_RF_MIN_ATTEN_MDB || tx1_atten_mdB%250U ||
		   ad9361_get_tx_attenuation(ad9361_phy,0U,&actual_atten)!=0 ||
		   actual_atten-P351_MANUAL_RF_MIN_ATTEN_MDB>89750U-P351_MANUAL_RF_MIN_ATTEN_MDB || actual_atten%250U ||
		   actual_atten!=tx1_atten_mdB) {
			(void)p351_rf_route(0U);
			if(ad9361_phy!=NULL) {
				(void)ad9361_en_dis_tx(ad9361_phy,TX_1|TX_2,DISABLE);
				(void)ad9361_tx_lo_powerdown(ad9361_phy,1U);
			}
			/* SPI写成功不等于衰减值已正确生效；合法但不等于请求的回读也必须停发。 */
			xil_printf("[manual-rf] refuse RF: attenuation/readback mismatch req=%u read=%u mdB; require exact 0..89.75dB/0.25dB match\r\n",
					(unsigned)tx1_atten_mdB, (unsigned)actual_atten);
			return XST_FAILURE;
		}
	}
#endif
	if (tx2_enable) {
		xil_printf("[tx-rf] reject second logical slot in 1R1T profile\r\n");
		return XST_FAILURE;
	}
	/* P351有板载PA/射频开关：先断开外部TX路径，再处理芯片门控。
	 * 未验收的新板禁止任何开RF请求，包括上位机/屏幕/上电自动发射。 */
	if (p351_rf_route(0U) != 0 ||
			(expected_channels && !P351_RF_HARDWARE_QUALIFIED)) {
		(void)ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
		(void)ad9361_tx_lo_powerdown(ad9361_phy, 1U);
		xil_printf("[p351] RF refused: hardware qualification or RF-switch readback\r\n");
		return XST_FAILURE;
	}
	/* Never leave the chip's diagnostic source overriding PL I/Q when RF is
	 * shut down or a new waveform/LO transaction begins. */
	/* Hardware state, not the software cache, decides whether PL owns TX.
	 * A failed BIST write may already have reached the device. */
	bist_status = expected_channels
			? ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG)
			: ((board_tx_bist_diag(0U) & 0xFFFFU) ? -1 : 0);
	if (bist_status < 0 || (expected_channels && (bist_status & BIST_ENABLE))) {
		(void)p351_rf_route(0U);
		(void) ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
		(void) ad9361_tx_lo_powerdown(ad9361_phy, 1U);
		xil_printf("[tx-rf] refuse: BIST not verified OFF; RF gated OFF\r\n");
		return XST_FAILURE;
	}
	if (expected_channels && (!p351_clock_ready() || tx_lvds_port_verify(0U) != XST_SUCCESS)) {
		(void)p351_rf_route(0U);
		(void)ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
		(void)ad9361_tx_lo_powerdown(ad9361_phy, 1U);
		return XST_FAILURE;
	}

	/* Wake the TX synthesizer before selecting an active TX path.  In the OFF
	 * direction the channels are removed first and the LO is powered down only
	 * after the last possible DAC/RF path has been gated. */
	lo_status = expected_channels ? ad9361_tx_lo_powerdown(ad9361_phy, 0U) : 0;

	if (fdd) {
		ensm_status = ad9361_set_en_state_machine_mode(ad9361_phy,
				ENSM_MODE_FDD);
		status1 = ad9361_en_dis_tx(ad9361_phy, TX_1,
				(expected_channels & TX_1) ? ENABLE : DISABLE);
		status2 = ad9361_en_dis_tx(ad9361_phy, TX_2,
				(expected_channels & TX_2) ? ENABLE : DISABLE);
	} else if (expected_channels) {
		ensm_status = ad9361_set_en_state_machine_mode(ad9361_phy,
				ENSM_MODE_TX);
		status1 = ad9361_en_dis_tx(ad9361_phy, TX_1,
				(expected_channels & TX_1) ? ENABLE : DISABLE);
		status2 = ad9361_en_dis_tx(ad9361_phy, TX_2,
				(expected_channels & TX_2) ? ENABLE : DISABLE);
	} else {
		status1 = ad9361_en_dis_tx(ad9361_phy, TX_1, DISABLE);
		status2 = ad9361_en_dis_tx(ad9361_phy, TX_2, DISABLE);
		ensm_status = ad9361_set_en_state_machine_mode(ad9361_phy,
				ENSM_MODE_RX);
	}
	if (!expected_channels)
		lo_status = ad9361_tx_lo_powerdown(ad9361_phy, 1U);
	else if (lo_status == 0) {
		u32 attempt;
		int32_t lock = -1;
		/* 板载PA路径仍关闭。必须等TX PLL真实锁定，不能只看ENSM和LO掉电位。 */
		for (attempt = 0U; attempt < 100U; ++attempt) {
			lock = ad9361_spi_read(ad9361_phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
			if (lock < 0 || (lock & VCO_LOCK)) break;
			usleep(50);
		}
		if (lock < 0 || !(lock & VCO_LOCK)) lo_status = XST_FAILURE;
	}
	reg = ad9361_spi_read(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL);
	lo_reg = ad9361_spi_read(ad9361_phy->spi,
	REG_TX_SYNTH_POWER_DOWN_OVERRIDE);
	state = ad9361_ensm_get_state(ad9361_phy);
	if (status1 != 0 || status2 != 0 || ensm_status != 0 || lo_status != 0
			|| reg < 0 || lo_reg < 0
			|| ((u8) reg & TX_CHANNEL_ENABLE(TX_1 | TX_2))
					!= TX_CHANNEL_ENABLE(expected_channels)
			|| state != expected_state
			|| (((u8) lo_reg & TX_LO_POWER_DOWN)
					!= (expected_channels ? 0U : TX_LO_POWER_DOWN))) {
		/* Best-effort fail-safe: remove both TX channels and leave RX alive. */
		(void)p351_rf_route(0U);
		ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
		ad9361_tx_lo_powerdown(ad9361_phy, 1U);
		ad9361_set_en_state_machine_mode(ad9361_phy,
				fdd ? ENSM_MODE_FDD : ENSM_MODE_RX);
		xil_printf(
				"[tx-rf] gate FAIL s=%d/%d ensm=%d lo=%d reg=0x%02x loreg=0x%02x state=%u exp=%u mask=0x%02x fdd=%u\r\n",
				(int) status1, (int) status2, (int) ensm_status,
				(int) lo_status, (unsigned) (reg & 0xFF),
				(unsigned) (lo_reg & 0xFF), (unsigned) state,
				(unsigned) expected_state,
				(unsigned) TX_CHANNEL_ENABLE(expected_channels),
				(unsigned) fdd);
		return XST_FAILURE;
	}
	if (p351_rf_route(expected_channels ? 1U : 0U) != 0) {
		(void)ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
		(void)ad9361_tx_lo_powerdown(ad9361_phy, 1U);
		return XST_FAILURE;
	}
	xil_printf(
			"[tx-rf] TX1=%s TX2=%s TXLO=%s ensm=%u reg=0x%02x loreg=0x%02x\r\n",
			(expected_channels & TX_1) ? "ON" : "OFF",
			(expected_channels & TX_2) ? "ON" : "OFF",
			expected_channels ? "ON" : "OFF", (unsigned) state,
			(unsigned) ((u8) reg), (unsigned) ((u8) lo_reg));
	return XST_SUCCESS;
}

/* 确认驱动映射及物理门控，不能只看软件缓存。
 * 允许初始化时选中的TX暂时开启以完成芯片校准；未选中TX必须关闭。 */
static int rf_channel_profile_verify(void) {
	int32_t rx, tx;
	if (!ad9361_phy || !ad9361_phy->pdata || ad9361_phy->pdata->rx2tx2 ||
			ad9361_phy->pdata->rx1tx1_mode_use_rx_num != RF_RX_PHYSICAL_CHANNEL ||
			ad9361_phy->pdata->rx1tx1_mode_use_tx_num != RF_TX_PHYSICAL_CHANNEL)
		return XST_FAILURE;
	rx = ad9361_spi_read(ad9361_phy->spi, REG_RX_ENABLE_FILTER_CTRL);
	tx = ad9361_spi_read(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL);
	if (rx < 0 || tx < 0 ||
			((u8)rx & RX_CHANNEL_ENABLE(RX_1 | RX_2)) != RX_CHANNEL_ENABLE(RF_RX_PHYSICAL_CHANNEL) ||
			((u8)tx & TX_CHANNEL_ENABLE((TX_1 | TX_2) ^ RF_TX_PHYSICAL_CHANNEL)))
		return XST_FAILURE;
	xil_printf("[rf-channel] PASS 1R1T logical0=RX%uA/TX%uA RXen=0x%02x TXen=0x%02x; other RF channels disabled\r\n",
			(unsigned)RF_RX_PHYSICAL_CHANNEL, (unsigned)RF_TX_PHYSICAL_CHANNEL,
			(unsigned)rx, (unsigned)tx);
	return XST_SUCCESS;
}

/* 低层API直接读物理1/2；高层ch=1在本1R1T工程中不是第二个物理端口。 */
static int32_t tx_physical_atten_read(u32 channel, u32 *attenuation) {
	int32_t value = ad9361_get_tx_atten(ad9361_phy, channel);
	if (value < 0)
		return value;
	*attenuation = (u32)value;
	return 0;
}

/* Configuration readback is useful for finding a failed SPI/setup sequence.
 * It cannot prove the 122.88 MHz LVDS eye or pin-level data integrity.
 * Repair is only called while both RF TX channels are gated OFF. */
static int tx_lvds_port_verify(u8 repair) {
	static const u16 regs[] = {
		REG_PARALLEL_PORT_CONF_1, REG_PARALLEL_PORT_CONF_2,
		REG_PARALLEL_PORT_CONF_3, REG_TX_CLOCK_DATA_DELAY,
		REG_LVDS_BIAS_CTRL, REG_LVDS_INVERT_CTRL1,
		REG_LVDS_INVERT_CTRL2
	};
	u8 expected[sizeof(regs) / sizeof(regs[0])];
	u32 i;
	int32_t value;
	struct ad9361_phy_platform_data *pd;
	if (!ad9361_phy || !ad9361_phy->spi || !ad9361_phy->pdata)
		return XST_FAILURE;
	pd = ad9361_phy->pdata;
	expected[0] = pd->port_ctrl.pp_conf[0];
	expected[1] = pd->port_ctrl.pp_conf[1];
	expected[2] = pd->port_ctrl.pp_conf[2];
	expected[3] = pd->port_ctrl.tx_clk_data_delay;
	expected[4] = pd->port_ctrl.lvds_bias_ctrl;
	expected[5] = pd->port_ctrl.lvds_invert[0];
	expected[6] = pd->port_ctrl.lvds_invert[1];
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
		value = ad9361_spi_read(ad9361_phy->spi, regs[i]);
		if (value < 0) {
			xil_printf("[tx-if] SPI read FAIL reg=0x%03x rc=%d\r\n",
					(unsigned) regs[i], (int) value);
			return XST_FAILURE;
		}
		if ((u8) value == expected[i])
			continue;
		xil_printf("[tx-if] mismatch reg=0x%03x got=0x%02x expected=0x%02x\r\n",
				(unsigned) regs[i], (unsigned) ((u8) value),
				(unsigned) expected[i]);
		if (!repair || ad9361_spi_write(ad9361_phy->spi, regs[i],
				expected[i]) < 0)
			return XST_FAILURE;
		value = ad9361_spi_read(ad9361_phy->spi, regs[i]);
		if (value < 0 || (u8) value != expected[i])
			return XST_FAILURE;
	}
	xil_printf("[tx-if] PASS C1/C2/C3 TXdelay bias invert1/2; pin eye not measured\r\n");
	return XST_SUCCESS;
}

static void tx_boot_fail(const char *stage, int32_t status) {
	xil_printf("[main] FATAL %s rc=%d; AD9361 reset asserted, RF OFF\r\n",
			stage, (int) status);
	Axi_Ctrl_Write(DDS_INC, 0U);
	gpio_set_value(GPIO_RESET_PIN, 0);
	for (;;) { }
}

u32 board_tx_bist_diag(u32 action) {
	int32_t status, readback, gate;
	u32 tone_level_db;
	XTime now;
	if (!ad9361_phy || action > 5U)
		return 3U;
	if (action == 2U)
		return tx_bist_active ? 0x00010000U : 0U;
	if (action == 0U) {
		/* Do not trust tx_bist_active after an interrupted/failed SPI write. */
		readback = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
		if (readback >= 0 && !(readback & BIST_ENABLE) && !tx_bist_active)
			return 0U;
		status = ad9361_bist_tone(ad9361_phy, BIST_DISABLE, 0U, 0U, 0U);
		readback = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
		if (status != 0 || readback < 0 || (readback & BIST_ENABLE)) {
			xil_printf("[tx-bist] disable FAIL rc=%d reg=0x%02x\r\n",
					(int) status, (unsigned) (readback & 0xFF));
			return 2U | 0x00010000U;
		}
		tx_bist_active = 0U;
		tx_bist_deadline = 0;
		tx_bist_retry_at = 0;
		tx_bist_disable_failures = 0U;
		xil_printf("[tx-bist] OFF; PL I/Q restored\r\n");
		return 0U;
	}
	/* 离线候选版本不能借BIST绕过板级射频联调门控。查询和关闭仍可用。 */
	if (!P351_RF_HARDWARE_QUALIFIED)
		return 3U;
	/* Restrict the intrusive hardware-isolation test to the already-on CW
	 * path at >=20 dB TX attenuation. The unselected RF channel stays disabled. Actions 1,3,4,5
	 * select -18,-0,-6,-12 dB internal levels respectively.  Keeping RF
	 * attenuation fixed exposes the onset of downstream compression. */
	gate = ad9361_spi_read(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL);
	if (tx1_mode_current != TX_MODE_CW
			|| tx2_mode_current
					!= 0U||
					tx1_atten_mdB < 20000U || !refclk_valid || gate < 0 ||
					((u8)gate & TX_CHANNEL_ENABLE(TX_1 | TX_2)) != TX_CHANNEL_ENABLE(RF_TX_PHYSICAL_CHANNEL))
		return 1U;
	tone_level_db = (action == 3U) ? 0U :
			(action == 4U) ? 6U : (action == 5U) ? 12U : 18U;
	status = ad9361_bist_tone(ad9361_phy, BIST_INJ_TX, 1920000U,
			tone_level_db, 0U);
	readback = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
	if (status != 0 || readback < 0 || !(readback & BIST_ENABLE)) {
		/* A failed enable can still leave BIST running (for example a
		 * readback failure after the write). Keep the watchdog armed until
		 * disable is verified; never silently orphan an internal tone. */
		XTime_GetTime(&now);
		tx_bist_active = 1U;
		tx_bist_deadline = now;
		tx_bist_retry_at = 0;
		tx_bist_disable_failures = 0U;
		(void) board_tx_bist_diag(0U);
		xil_printf("[tx-bist] enable FAIL rc=%d reg=0x%02x\r\n", (int) status,
				(unsigned) (readback & 0xFF));
		return 2U | (tx_bist_active ? 0x00010000U : 0U);
	}
	XTime_GetTime(&now);
	tx_bist_active = 1U;
	tx_bist_deadline = now + (XTime) 30U * COUNTS_PER_SECOND;
	tx_bist_retry_at = 0;
	tx_bist_disable_failures = 0U;
	xil_printf(
			"[tx-bist] ON 30s TX%u internal Fs/32=1.92MHz level=-%udB; AD9361 reg=0x%02x; PL LVDS bypassed\r\n",
			(unsigned) RF_TX_PHYSICAL_CHANNEL, (unsigned) tone_level_db,
			(unsigned) (readback & 0xFF));
	return 0x00010000U;
}

static void board_tx_bist_diag_service(void) {
	XTime now;
	if (!tx_bist_active)
		return;
	XTime_GetTime(&now);
	if (now >= tx_bist_deadline && now >= tx_bist_retry_at
			&& (board_tx_bist_diag(0U) & 0xFFFFU) != 0U) {
		++tx_bist_disable_failures;
		tx_bist_retry_at = now + COUNTS_PER_SECOND;
		if (tx_bist_disable_failures >= 3U) {
			/* If SPI cannot disable BIST, do not leave an uncontrolled RF
			 * diagnostic tone on indefinitely. Try the independent RF gates. */
			(void) ad9361_en_dis_tx(ad9361_phy, TX_1 | TX_2, DISABLE);
			(void) ad9361_tx_lo_powerdown(ad9361_phy, 1U);
			tx_bist_active = 0U;
			tx_bist_deadline = 0;
			xil_printf(
					"[tx-bist] emergency RF OFF after 3 disable failures; power-cycle/check SPI\r\n");
		}
	}
}

/* On-demand end-to-end transmitter snapshot requested by UI "TXST".
 * AXI DMA MM2S may already be idle after filling the PL circular BRAM, so
 * dma_busy=0 is not an error in infinite mode.  The decisive BIT criteria are
 * software active + engine BUSY + no underrun/error. */
/* Read-only 5 s TX health snapshot. No sleep, capture, RF write or recovery.
 * Drain at most 32 UART bytes per service call; a full FIFO never blocks.
 * Keep the interface register readback needed to compare good/bad boots.
 * Counters are diagnostics, not RF measurements or an IRIG acceptance verdict. */
static void tx_soak_service(void) {
	static char pending[640];
	static u32 pos, length, sequence, skipped, last_frames, last_sent;
	static u32 last_rate, last_bits, last_modes, idle_samples;
	static XTime last_report, last_call, max_gap;
	XTime now, elapsed, gap;
	dma_tx_bit_status_t bit;
	u32 modes, moved, budget = 32U;
	u32 att0 = 0U, att1 = 0U;
	int gate, pll, lo_pd, port0, port2, delay, bist, qec, att_rc0, att_rc1, n;
	XTime_GetTime(&now);
	if (!last_report) {
		last_report = now;
		last_call = now;
	}
	gap = now - last_call;
	last_call = now;
	if (gap > max_gap)
		max_gap = gap;
	while (pos < length && budget && !XUartPs_IsTransmitFull(STDOUT_BASEADDRESS)) {
		XUartPs_WriteReg(STDOUT_BASEADDRESS, XUARTPS_FIFO_OFFSET,
				(u8 )pending[pos++]);
		--budget;
	}
	elapsed = now - last_report;
	if (elapsed < (XTime) 5U * COUNTS_PER_SECOND)
		return;
	/* No catch-up bursts after a slow operation or debugger pause. */
	last_report = now;
	if (pos < length) {
		++skipped;
		return;
	}
	dma_tx_bit_get_status(&bit);
	modes = ((u32) tx2_mode_current << 8) | tx1_mode_current;
	moved = (bit.completed_frames != last_frames || bit.bits_sent != last_sent);
	if (bit.active && (bit.engine_status & 1U) && !moved && modes == last_modes
			&& bit.configured_bit_rate == last_rate
			&& bit.configured_bits == last_bits)
		++idle_samples;
	else
		idle_samples = 0U;
	gate = ad9361_spi_read(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL);
	pll = ad9361_spi_read(ad9361_phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
	lo_pd = ad9361_spi_read(ad9361_phy->spi, REG_TX_SYNTH_POWER_DOWN_OVERRIDE);
	port0 = ad9361_spi_read(ad9361_phy->spi, REG_PARALLEL_PORT_CONF_1);
	port2 = ad9361_spi_read(ad9361_phy->spi, REG_PARALLEL_PORT_CONF_3);
	delay = ad9361_spi_read(ad9361_phy->spi, REG_TX_CLOCK_DATA_DELAY);
	bist = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
	qec = ad9361_spi_read(ad9361_phy->spi, RF_TX_QEC_STATUS_REG);
	att_rc0 = tx_physical_atten_read(1U, &att0);
	att_rc1 = tx_physical_atten_read(2U, &att1);
	++sequence;
	n =
			snprintf(pending, sizeof(pending),
					"[tx-health %u] t=%us mode=%x LO=%uMHz ref=%u/%u valid=%u RF=%02x/%u pll=%u lo_pd=%d att=%u/%u spi=%d/%d IF=%02x/%02x delay=%02x bist=%02x qec=%02x\r\n"
							"[tx-health %u] bit=%u/%x frames=%u moved=%u idle=%u rate=%u err=%u net=%u/%u pend=%u bna=%u ovr=%u macerr=%u/%x irqfault=%u gap_us=%u skip=%u\r\n",
					(unsigned) sequence, (unsigned) (now / COUNTS_PER_SECOND),
					(unsigned) modes, (unsigned) (tx_lo_freq / 1000000ULL),
					(unsigned) refclk_source_current,
					(unsigned) refclk_rate_current, (unsigned) refclk_valid,
					(unsigned) (gate & 0xFF),
					(unsigned) ad9361_ensm_get_state(ad9361_phy),
					(unsigned) (pll >= 0 && (pll & VCO_LOCK)), lo_pd,
					(unsigned) att0, (unsigned) att1, att_rc0, att_rc1,
					(unsigned) (port0 & 0xFF), (unsigned) (port2 & 0xFF),
					(unsigned) (delay & 0xFF), (unsigned) (bist & 0xFF),
					(unsigned) (qec & 0xFF),
					(unsigned) sequence, (unsigned) bit.active,
					(unsigned) bit.engine_status,
					(unsigned) bit.completed_frames, (unsigned) moved,
					(unsigned) idle_samples, (unsigned) bit.configured_bit_rate,
					(unsigned) bit.error_code, (unsigned) eth_link_is_up(),
					(unsigned) udp_peer_ok(), (unsigned) eth_tx_pending(),
					(unsigned) eth_rx_buffer_errors(),
					(unsigned) eth_rx_overrun_errors(),
					(unsigned) eth_mac_error_count(),
					(unsigned) eth_mac_last_error(),
					(unsigned) eth_rx_irq_faults(),
					(unsigned) (max_gap / (COUNTS_PER_SECOND / 1000000U)),
					(unsigned) skipped);
	length = (n > 0 && (unsigned) n < sizeof(pending)) ? (u32) n : 0U;
	if (!length)
		++skipped;
	pos = 0U;
	max_gap = 0;
	last_frames = bit.completed_frames;
	last_sent = bit.bits_sent;
	last_rate = bit.configured_bit_rate;
	last_bits = bit.configured_bits;
	last_modes = modes;
}

static void tx_runtime_status_print(void) {
	dma_tx_bit_status_t bit;
	int32_t reg;
	u8 state;
	u8 expected_channels;
	u8 rf_ok;
	u8 bit_mode;
	u8 bit_ok;
	u8 waveform_ok;
	u8 attenuation_ok;
	u8 pll_ok;
	u32 dds_control;
	u32 pcm_control;
	u32 atten1_readback = 0U;
	u32 atten2_readback = 0U;
	int32_t atten1_status;
	int32_t atten2_status;
	int32_t tx_lock_reg;
	int32_t tx_lo_pd_reg;
	int32_t tx_quad_reg;
	int32_t tx_quad_phase_reg;
	int32_t tx_quad_atten_reg;
	int32_t tx_qec_corr[8];
	int32_t tx_qec_corr_ok = 1;
	static const u32 tx_qec_corr_regs[8] = {
		RF_TX_QEC_CORRECTION_REGS
	};
	u32 tx_qec_i;
	u8 tx_lo_ok;
	int32_t observe_reg;
	u8 loopback_hw;
	u8 loopback_ok;
	u32 monitor_source;
	u32 monitor_status;

	dma_tx_bit_get_status(&bit);
	reg = ad9361_spi_read(ad9361_phy->spi, REG_TX_ENABLE_FILTER_CTRL);
	state = ad9361_ensm_get_state(ad9361_phy);
	atten1_status = tx_physical_atten_read(1U, &atten1_readback);
	atten2_status = tx_physical_atten_read(2U, &atten2_readback);
	tx_lock_reg = ad9361_spi_read(ad9361_phy->spi,
	REG_TX_CP_OVERRANGE_VCO_LOCK);
	tx_lo_pd_reg = ad9361_spi_read(ad9361_phy->spi,
	REG_TX_SYNTH_POWER_DOWN_OVERRIDE);
	tx_quad_reg = ad9361_spi_read(ad9361_phy->spi,
	RF_TX_QEC_STATUS_REG);
	tx_quad_phase_reg = ad9361_spi_read(ad9361_phy->spi,
	REG_QUAD_CAL_NCO_FREQ_PHASE_OFFSET);
	tx_quad_atten_reg = ad9361_spi_read(ad9361_phy->spi,
	REG_TX_QUAD_CAL_ATTEN);
	for (tx_qec_i = 0U; tx_qec_i < 8U; ++tx_qec_i) {
		tx_qec_corr[tx_qec_i] = ad9361_spi_read(ad9361_phy->spi,
				tx_qec_corr_regs[tx_qec_i]);
		if (tx_qec_corr[tx_qec_i] < 0)
			tx_qec_corr_ok = 0;
	}
	observe_reg = ad9361_spi_read(ad9361_phy->spi, REG_OBSERVE_CONFIG);
	loopback_hw =
			(observe_reg >= 0 && ((u8) observe_reg & DATA_PORT_LOOP_TEST_ENABLE)) ?
					1U : 0U;
	loopback_ok =
			(observe_reg >= 0 && loopback_hw == tx_rx_loopback_current) ?
					1U : 0U;
	monitor_source = rx2r_get_source();
	monitor_status = rx2r_status();
	expected_channels = tx1_mode_current ? RF_TX_PHYSICAL_CHANNEL : 0U;
	attenuation_ok =
			(atten1_status == 0 && atten2_status == 0 &&
			 (RF_TX_PHYSICAL_CHANNEL == 1U ?
			  ((!tx1_mode_current || atten1_readback == tx1_atten_mdB) && atten2_readback == TX_MUTE_ATTEN_MDB) :
			  ((!tx1_mode_current || atten2_readback == tx1_atten_mdB) && atten1_readback == TX_MUTE_ATTEN_MDB))) ? 1U : 0U;
	pll_ok =
			(!expected_channels
					|| (tx_lock_reg >= 0 && ((u8) tx_lock_reg & VCO_LOCK))) ?
					1U : 0U;
	tx_lo_ok =
			(tx_lo_pd_reg >= 0
					&& (((u8) tx_lo_pd_reg & TX_LO_POWER_DOWN)
							== (expected_channels ? 0U : TX_LO_POWER_DOWN))) ?
					1U : 0U;
	rf_ok = (reg >= 0
			&& ((u8) reg & TX_CHANNEL_ENABLE(TX_1 | TX_2))
					== TX_CHANNEL_ENABLE(expected_channels)
			&& state
					== ((ad9361_phy->pdata->fdd) ?
							ENSM_STATE_FDD :
							(expected_channels ? ENSM_STATE_TX : ENSM_STATE_RX))
			&& attenuation_ok && pll_ok && tx_lo_ok) ? 1U : 0U;
	bit_mode =
			(tx_mode_is_bit(tx1_mode_current)
					|| tx_mode_is_bit(tx2_mode_current)) ? 1U : 0U;
	bit_ok =
			(!bit_mode
					|| (bit.active && (bit.engine_status & 0x1U)
							&& !(bit.engine_status & 0xCU))) ? 1U : 0U;
	dds_control = Xil_In32(AXI_CTRL_BASE_ADDR + DDS_INC);
	pcm_control = Xil_In32(AXI_CTRL_BASE_ADDR + PCM_CONTROL);
	waveform_ok =
			(dds_control
					== (((u32) tx1_mode_current << TX1_MODE_SHIFT)
							| ((u32) tx2_mode_current << TX2_MODE_SHIFT))
					&& (!bit_mode || (pcm_control & PCM_CTRL_ENABLE))) ?
					1U : 0U;

	xil_printf(
			"[tx-check] BIT %s active=%u busy=%u done=%u underrun=%u error=%u dma=%u sent=%u frames=%u bits=%u rate=%u st=0x%08x\r\n",
			(bit_ok && waveform_ok) ? "PASS" : "FAIL", (unsigned) bit.active,
			(unsigned) (bit.engine_status & 1U),
			(unsigned) ((bit.engine_status >> 1) & 1U),
			(unsigned) ((bit.engine_status >> 2) & 1U),
			(unsigned) ((bit.engine_status >> 3) & 1U), (unsigned) bit.dma_busy,
			(unsigned) bit.bits_sent, (unsigned) bit.completed_frames,
			(unsigned) bit.configured_bits, (unsigned) bit.configured_bit_rate,
			(unsigned) bit.engine_status);
	/* These are configuration/readback checks, not proof of RF modulation at
	 * the selected SMA.  The TX DAC monitor taps the PL side before LVDS. */
	xil_printf(
			"[tx-check] RF-CFG %s TX1=%s/%u TX2=%s/%u ensm=%u reg=0x%02x dds=0x%08x pcm=0x%08x LO=%uMHz BW=%uHz\r\n",
			(rf_ok && waveform_ok) ? "PASS" : "FAIL",
			(expected_channels & TX_1) ? "ON" : "OFF",
			(unsigned)((expected_channels & TX_1) ? tx1_mode_current : 0U),
			(expected_channels & TX_2) ? "ON" : "OFF",
			(unsigned)((expected_channels & TX_2) ? tx1_mode_current : 0U),
			(unsigned) state, (unsigned) (reg & 0xFF), (unsigned) dds_control,
			(unsigned) pcm_control, (unsigned) (tx_lo_freq / 1000000ULL),
			(unsigned) bandwidth);
	xil_printf(
			"[tx-check] PWR %s req=%u/%u mdB read=%u/%u mdB spi=%d/%d txlo=%s(0x%02x) txpll=%s(0x%02x)\r\n",
			(attenuation_ok && tx_lo_ok && pll_ok) ? "PASS" : "FAIL",
			(unsigned)(RF_TX_PHYSICAL_CHANNEL == 1U ? tx1_atten_mdB : TX_MUTE_ATTEN_MDB),
			(unsigned)(RF_TX_PHYSICAL_CHANNEL == 2U ? tx1_atten_mdB : TX_MUTE_ATTEN_MDB),
			(unsigned) atten1_readback, (unsigned) atten2_readback,
			(int) atten1_status, (int) atten2_status,
			(expected_channels && tx_lo_ok) ? "ON" :
			(!expected_channels && tx_lo_ok) ? "OFF" : "BAD",
			(unsigned) (tx_lo_pd_reg & 0xFF), pll_ok ? "LOCK" : "UNLOCK",
			(unsigned) (tx_lock_reg & 0xFF));
	xil_printf(
			"[tx-check] IF port=0x%02x/0x%02x tx_delay=0x%02x bist=0x%02x diag=%u (TX DAC monitor is before LVDS)\r\n",
			(unsigned) (ad9361_spi_read(ad9361_phy->spi,
					REG_PARALLEL_PORT_CONF_1) & 0xFF),
			(unsigned) (ad9361_spi_read(ad9361_phy->spi,
					REG_PARALLEL_PORT_CONF_2) & 0xFF),
			(unsigned) (ad9361_spi_read(ad9361_phy->spi,
					REG_TX_CLOCK_DATA_DELAY) & 0xFF),
			(unsigned) (ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG) & 0xFF),
			(unsigned) tx_bist_active);
	/* Convergence flags are calibration diagnostics, not an RF image-rejection
	 * measurement. Compare the spectrum at the selected SMA for suppression. */
	xil_printf(
			"[tx-check] QEC %s TX%u=0x%02x LO_conv=%u SSB_conv=%u phase_cache=%u phase_hw=%d cal_att=0x%02x cal_LO=%uMHz (RF spectrum required)\r\n",
			(tx_quad_reg >= 0 &&
					((u8) tx_quad_reg & (RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV))
					== (RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV)) ? "CONV" : "WARN",
			(unsigned) RF_TX_PHYSICAL_CHANNEL,
			(unsigned) (tx_quad_reg & 0xFF),
			(unsigned) (tx_quad_reg >= 0 && ((u8) tx_quad_reg & RF_TX_QEC_LO_CONV) != 0U),
			(unsigned) (tx_quad_reg >= 0 && ((u8) tx_quad_reg & RF_TX_QEC_SSB_CONV) != 0U),
			(unsigned) ad9361_phy->last_tx_quad_cal_phase,
			(int) (tx_quad_phase_reg < 0 ? tx_quad_phase_reg : (tx_quad_phase_reg & 0x1F)),
			(unsigned) (tx_quad_atten_reg & 0xFF),
			(unsigned) (ad9361_phy->last_tx_quad_cal_freq / 1000000ULL));
	xil_printf(
			"[tx-check] QEC-COEFF %s TX%u_out1 phase/gain/I/Q=%02x/%02x/%02x/%02x out2=%02x/%02x/%02x/%02x (raw SPI; compare good/bad boots)\r\n",
			tx_qec_corr_ok ? "READ" : "SPI-ERR",
			(unsigned) RF_TX_PHYSICAL_CHANNEL,
			(unsigned) (tx_qec_corr[0] & 0xFF),
			(unsigned) (tx_qec_corr[1] & 0xFF),
			(unsigned) (tx_qec_corr[2] & 0xFF),
			(unsigned) (tx_qec_corr[3] & 0xFF),
			(unsigned) (tx_qec_corr[4] & 0xFF),
			(unsigned) (tx_qec_corr[5] & 0xFF),
			(unsigned) (tx_qec_corr[6] & 0xFF),
			(unsigned) (tx_qec_corr[7] & 0xFF));
	xil_printf(
			"[tx-check] LOOP %s requested=%u hw=%u obs=0x%02x ensm=%u RXfs=%u TXfs=%u RXLO=%uMHz TXLO=%uMHz path=%s\r\n",
			loopback_ok ? "PASS" : "FAIL", (unsigned) tx_rx_loopback_current,
			(unsigned) loopback_hw, (unsigned) (observe_reg & 0xFF),
			(unsigned) state, (unsigned) sample_rate, (unsigned) sample_rate,
			(unsigned) (rx_lo_freq / 1000000ULL),
			(unsigned) (tx_lo_freq / 1000000ULL),
			loopback_hw ? "DIGITAL(no-LO-limit)" : "RF-RX");
	xil_printf(
			"[tx-check] MON %s request=%s active=%s tx_seen=%u rx_seen=%u rf_rx=%u armed=%u st=0x%08x\r\n",
			(((monitor_status & RX2R_ST_SOURCE_TX)
					== (monitor_source ? RX2R_ST_SOURCE_TX : 0U))
					&& (monitor_source ?
							((monitor_status & RX2R_ST_TX_SEEN) != 0U) :
							(rx_rf_monitor_current
									&& (monitor_status & RX2R_ST_RX_SEEN) != 0U))) ?
					"PASS" : "WAIT", monitor_source ? "TX_DAC" : "RX_ADC",
			(monitor_status & RX2R_ST_SOURCE_TX) ? "TX_DAC" : "RX_ADC",
			(unsigned) ((monitor_status & RX2R_ST_TX_SEEN) ? 1U : 0U),
			(unsigned) ((monitor_status & RX2R_ST_RX_SEEN) ? 1U : 0U),
			(unsigned) rx_rf_monitor_current,
			(unsigned) ((monitor_status & RX2R_ST_ARMED) ? 1U : 0U),
			(unsigned) monitor_status);
	xil_printf(
			"[tx-check] NET %s link=%u tx_pending=%u/%u tx_done=%u bd_full=%u counter_fault=%u rx_hw=%u/%u rx_q=%u rx_drop=%u rx_bna=%u rx_ovr=%u rx_recover=%u cap=%u stream_err=%u dma_sr=0x%08x dma_timeout=%u recovery=%u\r\n",
			(eth_link_is_up() && eth_tx_pending() < TX_BD_CNT
					&& eth_rx_hw_owned() == RX_BD_CNT
					&& !(dma_rx_hw_status() & XAXIDMA_IRQ_ERROR_MASK)) ?
					"PASS" : "WARN", (unsigned) eth_link_is_up(),
			(unsigned) eth_tx_pending(), (unsigned) TX_BD_CNT,
			(unsigned) eth_tx_completed(), (unsigned) eth_tx_alloc_failures(),
			(unsigned) eth_tx_counter_faults(), (unsigned) eth_rx_hw_owned(),
			(unsigned) RX_BD_CNT, (unsigned) eth_rx_queue_depth(),
			(unsigned) eth_rx_sw_drops(), (unsigned) eth_rx_buffer_errors(),
			(unsigned) eth_rx_overrun_errors(), (unsigned) eth_rx_recoveries(),
			(unsigned) udp_stream_captures(), (unsigned) udp_stream_errors(),
			(unsigned) dma_rx_hw_status(), (unsigned) dma_rx_timeout_count(),
			(unsigned) dma_rx_recovery_count());
	udp_stream_iq_print();
	xil_printf("[tx-check] ROUTE TRX2_TX=%u RX2_SEL=%u ext_req=%u ext_qualified=%u (PL readback, not SMA power)\r\n",
		(unsigned)gpio_get_value(P351_GPIO_TRX2_READBACK),
		(unsigned)gpio_get_value(P351_GPIO_RX2_READBACK),
		(unsigned)gpio_get_value(P351_GPIO_REF_REQUIRED),
		(unsigned)gpio_get_value(P351_GPIO_REF_QUALIFIED));
}

/* 115200串口的TXST多行输出可阻塞数十毫秒，超过20M位流FIFO预算。
 * 接收运行/停止未确认时仅记住请求，停止后输出最近一次；不占用接收服务。 */
static void tx_status_service(u8 requested) {
	static u8 deferred;
	if (requested) deferred=1U;
	if (deferred && !rx_receiver_active() && !board_final_rf_owned()) {
		deferred=0U;
		tx_runtime_status_print();
	}
}

static void tx_rx_loopback_apply(u8 mode) {
	int32_t status;
	int32_t readback = -1;
	int32_t observe_reg;
	u8 hardware_mode;
	u32 rx_fs = 0U;
	u32 tx_fs = 0U;
	u64 rx_lo = 0ULL;
	u64 tx_lo = 0ULL;

	if (mode > 1U) {
		xil_printf("[loopback] rejected mode=%u\r\n", (unsigned) mode);
		return;
	}
	if(mode && rx_receiver_active()){
		xil_printf("[loopback] BUSY real RF receiver; stop RXC1 before diagnostic loop\r\n");
		return;
	}
	status = ad9361_bist_loopback(ad9361_phy, mode);
	ad9361_get_bist_loopback(ad9361_phy, &readback);
	observe_reg = ad9361_spi_read(ad9361_phy->spi, REG_OBSERVE_CONFIG);
	hardware_mode =
			(observe_reg >= 0 && ((u8) observe_reg & DATA_PORT_LOOP_TEST_ENABLE)) ?
					1U : 0U;
	if (status != 0 || readback != (int32_t) mode || observe_reg < 0
			|| hardware_mode != mode) {
		/* A failed enable must not leave an unknown diagnostic route active. */
		ad9361_bist_loopback(ad9361_phy, 0);
		tx_rx_loopback_current = 0;
		xil_printf(
				"[loopback] FAIL req=%u st=%d cache=%d hw=%u obs=0x%02x; forced OFF\r\n",
				(unsigned) mode, (int) status, (int) readback,
				(unsigned) hardware_mode, (unsigned) (observe_reg & 0xFF));
		return;
	}
	tx_rx_loopback_current = mode;
	ad9361_get_rx_sampling_freq(ad9361_phy, &rx_fs);
	ad9361_get_tx_sampling_freq(ad9361_phy, &tx_fs);
	ad9361_get_rx_lo_freq(ad9361_phy, &rx_lo);
	ad9361_get_tx_lo_freq(ad9361_phy, &tx_lo);
	xil_printf(
			"[loopback] %s cache=%d hw=%u obs=0x%02x RXfs=%u TXfs=%u RXLO=%uMHz TXLO=%uMHz; source=%s\r\n",
			mode ? "ON TX->RX" : "OFF normal RX", (int) readback,
			(unsigned) hardware_mode, (unsigned) (observe_reg & 0xFF),
			(unsigned) rx_fs, (unsigned) tx_fs, (unsigned) (rx_lo / 1000000ULL),
			(unsigned) (tx_lo / 1000000ULL),
			mode ? "digital TX I/Q (LO bypassed)" : "RF RX I/Q");
}

/* Tune physical RX to the TX carrier for a coax self-test. */
static int rx_rf_tune_checked(u64 lo_hz, u32 bw_hz, int32_t gain0,
		int32_t gain1) {
	int32_t status;
	int32_t pll = -1;
	u64 lo_read = 0ULL;
	u32 bw_read = 0U;
	int32_t gain0_read = -1;
	(void) gain1; /* 第二逻辑槽位不存在；gain0经驱动映射到物理RX2。 */
	status = ad9361_set_rx_lo_freq(ad9361_phy, lo_hz);
	if (status == 0)
		status = ad9361_set_rx_rf_bandwidth(ad9361_phy, bw_hz);
	if (status == 0)
		status = ad9361_set_rx_rf_gain(ad9361_phy, 0U, gain0);
	if (status == 0)
		status = ad9361_get_rx_lo_freq(ad9361_phy, &lo_read);
	if (status == 0)
		status = ad9361_get_rx_rf_bandwidth(ad9361_phy, &bw_read);
	if (status == 0)
		status = ad9361_get_rx_rf_gain(ad9361_phy, 0U, &gain0_read);
	if (status == 0)
		pll = ad9361_spi_read(ad9361_phy->spi, REG_RX_CP_OVERRANGE_VCO_LOCK);
	/* RX与TX采用同样的PLL数字量化容差；5.137GHz回算差2Hz不应误报。 */
	if (status != 0 || (lo_read > lo_hz ? lo_read - lo_hz : lo_hz - lo_read) > 10ULL ||
			pll < 0 || !(pll & VCO_LOCK) || bw_read != bw_hz
			|| gain0_read != gain0) {
		xil_printf(
			"[rx-rf] tune FAIL rc=%d LO=%u/%uMHz BW=%u/%uHz gain=%d/%d\r\n",
				(int) status, (unsigned) (lo_hz / 1000000ULL),
				(unsigned) (lo_read / 1000000ULL), (unsigned) bw_hz,
				(unsigned) bw_read, (int) gain0, (int) gain0_read);
		return XST_FAILURE;
	}
	rx_lo_applied_hz = lo_read;
	rx_bw_applied_hz = bw_read;
	return XST_SUCCESS;
}

static void rx_rf_monitor_apply(u8 enable) {
	int32_t observe_reg;
	if(rx_receiver_active()){
		xil_printf("[rx-rf] BUSY independent receiver; stop RXC1 before legacy SMA monitor\r\n");
		return;
	}
	if (enable > 1U) {
		xil_printf("[rx-rf] rejected enable=%u\r\n", (unsigned) enable);
		return;
	}
	if (enable) {
		if (!lvds_cal_result.valid) {
			xil_printf("[rx-rf] refuse: RX LVDS calibration is not PASS; check PRBS/IDELAY/FRAME first\r\n");
			return;
		}
		/* This is a physical RF receive test, never the AD9361 digital loop. */
		if (tx_bist_active && (board_tx_bist_diag(0U) & 0xFFFFU) != 0U) {
			xil_printf("[rx-rf] refuse: AD9361 TX BIST could not be disabled\r\n");
			return;
		}
		observe_reg = ad9361_spi_read(ad9361_phy->spi, REG_OBSERVE_CONFIG);
		if (tx_rx_loopback_current || observe_reg < 0
				|| ((u8) observe_reg & DATA_PORT_LOOP_TEST_ENABLE))
			tx_rx_loopback_apply(0U);
		observe_reg = ad9361_spi_read(ad9361_phy->spi, REG_OBSERVE_CONFIG);
		if (observe_reg < 0
				|| ((u8) observe_reg & DATA_PORT_LOOP_TEST_ENABLE)) {
			xil_printf("[rx-rf] refuse: internal digital loopback remains enabled\r\n");
			return;
		}
		if (rx_rf_tune_checked(tx_lo_freq, bandwidth,
				RX_RF_MONITOR_GAIN_DB, RX_RF_MONITOR_GAIN_DB) != XST_SUCCESS) {
			rx_rf_monitor_current = 0U;
			(void) rx_rf_tune_checked(rx_lo_freq, rx_bandwidth_current,
					gain_rx1, gain_rx2);
			(void) rx2r_set_source(RX2R_SOURCE_TX_DAC);
			xil_printf("[rx-rf] external receive unavailable; RX source not selected\r\n");
			return;
		}
		rx_rf_monitor_current = 1U;
		if (rx2r_set_source(RX2R_SOURCE_RX_ADC) != XST_SUCCESS) {
			rx_rf_monitor_current = 0U;
			(void) rx_rf_tune_checked(rx_lo_freq, rx_bandwidth_current,
					gain_rx1, gain_rx2);
			(void) rx2r_set_source(RX2R_SOURCE_TX_DAC);
			xil_printf("[rx-rf] RX ADC source select failed; restored default RX profile\r\n");
			return;
		}
		xil_printf(
				"[rx-rf] ON external RX%u LO=%uMHz BW=%uHz gain=%ddB Fs=%u\r\n",
				(unsigned) RF_RX_PHYSICAL_CHANNEL,
				(unsigned) (tx_lo_freq / 1000000ULL), (unsigned) bandwidth,
				RX_RF_MONITOR_GAIN_DB,
				(unsigned) sample_rate);
		return;
	}
	if (rx_rf_monitor_current
			&& rx_rf_tune_checked(rx_lo_freq, rx_bandwidth_current, gain_rx1,
					gain_rx2) != XST_SUCCESS) {
		xil_printf("[rx-rf] restore default RX tune failed; monitor remains marked active\r\n");
		return;
	}
	rx_rf_monitor_current = 0U;
	(void) rx2r_set_source(RX2R_SOURCE_TX_DAC);
	xil_printf("[rx-rf] OFF; restored RXLO=%uMHz BW=%uHz gain=%ddB/%ddB\r\n",
			(unsigned) (rx_lo_freq / 1000000ULL),
			(unsigned) rx_bandwidth_current, gain_rx1, gain_rx2);
}

/* 模式切换：先关RF，再切PL/功率，最后开物理TX2；第二逻辑槽位拒绝启用。 */
u32 board_receiver_can_apply(const rx_receiver_config_t *cfg)
{
	int32_t bist,observe;
	u64 delta=cfg->center_hz>rx_lo_applied_hz?cfg->center_hz-rx_lo_applied_hz:rx_lo_applied_hz-cfg->center_hz;
	if(!refclk_valid || !p351_clock_ready() || !lvds_cal_result.valid || sample_rate!=61440000U)return 3;
	if(rx_rf_monitor_current || udp_stream_active() || (rx2r_status()&RX2R_ST_ARMED))return 3;
	if((delta>10ULL || cfg->bandwidth_hz!=rx_bw_applied_hz) &&
			(tx1_mode_current || tx2_mode_current || tx_bist_active))return 3;
	bist=ad9361_spi_read(ad9361_phy->spi,REG_BIST_CONFIG);
	observe=ad9361_spi_read(ad9361_phy->spi,REG_OBSERVE_CONFIG);
	if(bist!=0 || observe<0 || ((u8)observe&DATA_PORT_LOOP_TEST_ENABLE))return 3;
	return 0;
}

u32 board_receiver_tune(const rx_receiver_config_t *cfg)
{
	u32 fs=0,bw=0;u64 lo=0;int32_t pll;
	if(tx1_mode_current || tx2_mode_current){
		int32_t gain=0;
		/* 前置检查已证明LO/BW不变；不要用一次多余调谐使运行中TX进入ALERT。 */
		if(ad9361_set_rx_rf_gain(ad9361_phy,0,cfg->gain_db)!=0 ||
		   ad9361_get_rx_rf_gain(ad9361_phy,0,&gain)!=0 || gain!=cfg->gain_db)return 4;
	}else if(rx_rf_tune_checked(cfg->center_hz,cfg->bandwidth_hz,cfg->gain_db,0)!=XST_SUCCESS)return 4;
	if(ad9361_get_rx_sampling_freq(ad9361_phy,&fs)!=0 || fs!=61440000U)return 4;
	if(ad9361_get_rx_lo_freq(ad9361_phy,&lo)!=0 ||
	   ad9361_get_rx_rf_bandwidth(ad9361_phy,&bw)!=0 || bw!=cfg->bandwidth_hz ||
	   (lo>cfg->center_hz?lo-cfg->center_hz:cfg->center_hz-lo)>10ULL)return 4;
	pll=ad9361_spi_read(ad9361_phy->spi,REG_RX_CP_OVERRANGE_VCO_LOCK);
	if(pll<0 || !(pll&VCO_LOCK))return 4;
	/* 独立RX2不跟随TX换频；不调用TX门控或改变TX缓存。 */
	rx_rf_monitor_current=0;
	return 0;
}

static void tx_apply_modes(u8 tx1_mode, u8 tx2_mode) {
	u32 control;
	u8 target_bit_mode;
	u8 current_bit_mode;
	u8 modes_changed;
	u8 bit_mode_changed;
	int quiesce_status;
	int32_t atten1_status = 0;
	int32_t atten2_status = 0;
#if P351_MANUAL_RF_TEST
	if((tx1_mode || tx2_mode) &&
	   (tx1_atten_mdB-P351_MANUAL_RF_MIN_ATTEN_MDB>89750U-P351_MANUAL_RF_MIN_ATTEN_MDB || tx1_atten_mdB%250U)) {
		xil_printf("[manual-rf] mode rejected: invalid attenuation; use complete TXCF\r\n");
		return;
	}
#endif
	if ((!refclk_valid || tx_rf_config_inhibited) && (tx1_mode || tx2_mode)) {
		xil_printf("[tx] rejected: reference/RF config not verified; apply TXCF to recover\r\n");
		return;
	}
	if (!tx_mode_valid(tx1_mode) || !tx_mode_valid(tx2_mode)
			|| tx2_mode != TX_MODE_OFF || tx_modes_mixed(tx1_mode, tx2_mode))
		return;
	modes_changed =
			(tx1_mode != tx1_mode_current || tx2_mode != tx2_mode_current) ?
					1U : 0U;
	/* A mode write is a transaction boundary.  Stop the old infinite BIT
	 * stream before touching DDS/standard-modulator controls; otherwise the
	 * previous DMA frame can keep the PL busy and mask the new waveform. */
	if (modes_changed) {
		quiesce_status = dma_tx_bit_quiesce();
		if (quiesce_status != XST_SUCCESS) {
			xil_printf("[tx] BIT quiesce failed; mode change aborted\r\n");
			goto mode_failed;
		}
		tx_bit_protocol_mode_change();
	}
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS) {
		goto mode_failed;
	}
	/* All standard BIT modulators share one input stream and enable/clear
	 * register.  Keep enable low while changing the selected modulator. */
	target_bit_mode =
			tx_mode_is_bit(tx1_mode) ?
					tx1_mode :
					(tx_mode_is_bit(tx2_mode) ? tx2_mode : TX_MODE_OFF);
	current_bit_mode =
			tx_mode_is_bit(tx1_mode_current) ?
					tx1_mode_current :
					(tx_mode_is_bit(tx2_mode_current) ?
							tx2_mode_current : TX_MODE_OFF);
	bit_mode_changed =
			(modes_changed && target_bit_mode != TX_MODE_OFF
					&& target_bit_mode != current_bit_mode) ? 1U : 0U;
	if (modes_changed && !Axi_Ctrl_Write_Checked(PCM_CONTROL, shape_control_word()))
		goto mode_failed;
	control = ((u32) tx1_mode << TX1_MODE_SHIFT)
			| ((u32) tx2_mode << TX2_MODE_SHIFT);
	if (!Axi_Ctrl_Write_Checked(DDS_INC, control))
		goto mode_failed;
	/* TXCF and modulation-parameter commands are independent UDP datagrams.
	 * They can be serviced in either order.  A mode change deliberately clears
	 * the shared modulator control above, so restore the cached configuration
	 * here when the parameter datagram arrived first; otherwise the selected
	 * BIT mode remains enabled in software but disabled in PL. */
	if (bit_mode_changed) {
		if (target_bit_mode == TX_MODE_PCM_FM && pcm_enable_current) {
			pcm_apply_config(pcm_bit_rate_word_current,
					pcm_deviation_word_current,
					pcm_frequency_offset_word_current, pcm_amplitude_current,
					pcm_filter_profile_current, pcm_randomizer_current, 1U);
			if (!pcm_enable_current)
				return; /* A failed restore must not reopen RF below. */
		} else if (target_bit_mode == TX_MODE_SOQPSK_TG && soqpsk_enable_current) {
			soqpsk_apply_config(soqpsk_bit_rate_word_current,
					soqpsk_frequency_offset_word_current,
					soqpsk_amplitude_current, soqpsk_differential_current, 1U,
					1U);
			if (!soqpsk_enable_current)
				return;
		}
	}
	/* Only the selected standard modulator may retain its cached enable.  The
	 * old implementation left the other flags/guard latched, so a later mode
	 * switch could select a disabled PL path or reject every TXB1 START. */
	if (modes_changed) {
		if (target_bit_mode != TX_MODE_PCM_FM)
			pcm_enable_current = 0U;
		if (target_bit_mode != TX_MODE_SOQPSK_TG)
			soqpsk_enable_current = 0U;
		if ((target_bit_mode == TX_MODE_PCM_FM && !pcm_enable_current)
				|| (target_bit_mode == TX_MODE_SOQPSK_TG && !soqpsk_enable_current))
			tx_bit_protocol_set_standard_guard(TXB1_GUARD_BLOCKED, 0U);
		else if (target_bit_mode == TX_MODE_OFF)
			tx_bit_protocol_set_standard_guard(0U, 0U);
	}
	if (!tx_mode_is_bit(tx1_mode) && !tx_mode_is_bit(tx2_mode)) {
		if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, shape_control_word()))
			goto mode_failed;
		pcm_enable_current = 0U;
		soqpsk_enable_current = 0U;
		tx_bit_protocol_set_standard_guard(0U, 0U);
	}
#if TX_CW_MIN_ATTEN_MDB > 0
	if (tx1_mode == 6U && tx1_atten_mdB < TX_CW_MIN_ATTEN_MDB) {
		xil_printf("[tx-cw] TX%u attenuation %u -> %u mdB (linearity guard)\r\n",
			(unsigned)RF_TX_PHYSICAL_CHANNEL,
			(unsigned)tx1_atten_mdB, (unsigned)TX_CW_MIN_ATTEN_MDB);
		tx1_atten_mdB = TX_CW_MIN_ATTEN_MDB;
	}
#endif
	/* 未选中的物理TX始终最大衰减；不能调用高层ch=1假装设置另一物理通道。 */
	atten2_status = ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB,
			RF_TX_PHYSICAL_CHANNEL != TX_1, RF_TX_PHYSICAL_CHANNEL != TX_2, true);
	if (tx1_mode != 0U)
		atten1_status = ad9361_set_tx_attenuation(ad9361_phy, 0, tx1_atten_mdB);
	if (atten1_status != 0|| atten2_status != 0 ||
	tx_rf_gate_apply(tx1_mode != 0U, tx2_mode != 0U) != XST_SUCCESS) {
		xil_printf("[tx] power/gate apply failed att=%d/%d; RF remains OFF\r\n",
				(int) atten1_status, (int) atten2_status);
		goto mode_failed;
	}
	tx1_mode_current = tx1_mode;
	tx2_mode_current = tx2_mode;
	/* Keep the local screen aligned with changes made by either control path.
	 * This is a software state indication, not a spectrum/power readback. */
	if (tx1_mode_current == TX_MODE_OFF && tx2_mode_current == TX_MODE_OFF)
		screen_uart0_set_status("RF OFF");
	else if (tx1_mode_current == TX_MODE_CW)
		screen_uart0_set_status("CW ON");
	else if (tx1_mode_current == TX_MODE_PCM_FM)
		screen_uart0_set_status("PCM/FM");
	else if (tx1_mode_current == TX_MODE_SOQPSK_TG)
		screen_uart0_set_status("SOQPSK");
	else if (tx2_mode_current != TX_MODE_OFF)
		screen_uart0_set_status("TX2 ON");
	xil_printf("[tx] logical0->TX%uA mode=%u/%u.%03udB logical1=OFF RF=%s reg=0x%08x\r\n",
			(unsigned) RF_TX_PHYSICAL_CHANNEL,
			(unsigned) tx1_mode_current, (unsigned) (tx1_atten_mdB / 1000),
			(unsigned) (tx1_atten_mdB % 1000), tx1_mode_current ? "ON" : "OFF",
			(unsigned) control);
	return;
mode_failed:
	modulation_config_fail_closed();
	xil_printf("[tx] mode transaction failed; task aborted, full TXCF required\r\n");
}

/* 已关闭RF、已清零PL之后调用。只临时开启选中物理TX用于内部QEC。
 * 普通TX衰减不能屏蔽内部校准音！若要求换频时SMA绝对静默，须外置RF开关。
 * 驱动原有「TX未启用不可校准」保护保留；在这里补齐校准前置条件。 */
static u32 tx_retune_checked(u64 lo_hz, u32 bw_hz) {
	int32_t status = 0, qec, pll, bist;
	int32_t att1, att2;
	u64 lo_read = 0ULL;
	u64 delta = lo_hz > ad9361_phy->last_tx_quad_cal_freq ?
			lo_hz - ad9361_phy->last_tx_quad_cal_freq :
			ad9361_phy->last_tx_quad_cal_freq - lo_hz;
	u32 bw_read = 0U;
	u8 bw_changed = tx_rf_retune_failed || bw_hz != ad9361_phy->current_tx_bw_Hz;
	u8 saved_auto = ad9361_phy->auto_cal_en ? 1U : 0U;
	u8 needs_qec = 0U;
	const char *stage = "mute";
#define RETUNE_TRY(call) do { status = (call); if (status != 0) goto retune_exit; } while (0)
	RETUNE_TRY(rf_channel_profile_verify());
	RETUNE_TRY(ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB, true, true, true));
	att1 = ad9361_get_tx_atten(ad9361_phy, TX_1);
	att2 = ad9361_get_tx_atten(ad9361_phy, TX_2);
	if (att1 != TX_MUTE_ATTEN_MDB || att2 != TX_MUTE_ATTEN_MDB) {
		status = -1;
		goto retune_exit;
	}
	stage = "qec_precheck";
	qec = ad9361_spi_read(ad9361_phy->spi, RF_TX_QEC_STATUS_REG);
	if (qec < 0) {
		status = qec;
		goto retune_exit;
	}
	needs_qec = tx_rf_retune_failed || bw_changed ||
			delta > ad9361_phy->cal_threshold_freq ||
			((u8)qec & (RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV)) !=
					(RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV);
	stage = "cal_path";
	if (needs_qec) {
		xil_printf("[tx-retune] CAL TX%u LO=%uMHz BW=%uHz; PL OFF, att=89.75dB; internal calibration tone may reach SMA\r\n",
				(unsigned)RF_TX_PHYSICAL_CHANNEL,
				(unsigned)(lo_hz / 1000000ULL), (unsigned)bw_hz);
		RETUNE_TRY(tx_rf_gate_apply(1U, 0U));
	} else {
		/* 小跨度也要唤醒LO验证锁定，但不需要打开TX数据通道。 */
		RETUNE_TRY(ad9361_tx_lo_powerdown(ad9361_phy, 0U));
	}
	/* 由本事务负责QEC，避免LO和带宽同时改变时重复校准；原开关退出时恢复。 */
	stage = "auto_cal";
	RETUNE_TRY(ad9361_set_tx_auto_cal_en_dis(ad9361_phy, DISABLE));
	stage = "LO";
	RETUNE_TRY(ad9361_set_tx_lo_freq(ad9361_phy, lo_hz));
	stage = "bandwidth";
	/* 上次失败可能只写了一部分滤波寄存器；不能凭旧缓存跳过重建。
	 * 0是无效带宽标记，强制公开API重写TX滤波并做QEC；失败时RF继续锁止。 */
	if (tx_rf_retune_failed)
		ad9361_phy->current_tx_bw_Hz = 0U;
	RETUNE_TRY(ad9361_set_tx_rf_bandwidth(ad9361_phy, bw_hz));
	/* 带宽变化的驱动接口已在新LO执行QEC；否则显式执行一次。 */
	if (needs_qec && !bw_changed) {
		stage = "QEC";
		RETUNE_TRY(ad9361_do_calib_run(ad9361_phy, TX_QUAD_CAL, -1));
	}
	stage = "readback";
	RETUNE_TRY(ad9361_get_tx_lo_freq(ad9361_phy, &lo_read));
	RETUNE_TRY(ad9361_get_tx_rf_bandwidth(ad9361_phy, &bw_read));
	pll = ad9361_spi_read(ad9361_phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
	qec = ad9361_spi_read(ad9361_phy->spi, RF_TX_QEC_STATUS_REG);
	bist = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
	/* PLL整数/小数分频有量化误差；10Hz容差不是精度保证，更不是相位同步证明。 */
	if ((lo_read > lo_hz ? lo_read - lo_hz : lo_hz - lo_read) > 10ULL ||
			bw_read != bw_hz || pll < 0 || !(pll & VCO_LOCK) ||
			qec < 0 || ((u8)qec & (RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV)) !=
					(RF_TX_QEC_LO_CONV | RF_TX_QEC_SSB_CONV) ||
			bist < 0 || (bist & BIST_ENABLE)) {
		status = -1;
		goto retune_exit;
	}
retune_exit:
	/* 包括半途写失败：不恢复用户功率，不自动回跳旧LO，不允许旧模式重新开RF。 */
	if (ad9361_set_tx_auto_cal_en_dis(ad9361_phy, saved_auto) != 0)
		status = -1;
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS)
		status = -1;
	if (status != 0) {
		(void)ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB, true, true, true);
		xil_printf("[tx-retune] FAIL stage=%s rc=%d; RF OFF, complete TXCF required\r\n",
				stage, (int)status);
		return XST_FAILURE;
	}
	if (needs_qec)
		ad9361_phy->last_tx_quad_cal_freq = lo_read;
	xil_printf("[tx-retune] PASS LO=%uMHz BW=%uHz QEC=%s; RF OFF until final mode commit\r\n",
			(unsigned)(lo_read / 1000000ULL), (unsigned)bw_read,
			needs_qec ? "recalibrated" : "retained");
#undef RETUNE_TRY
	return XST_SUCCESS;
}

/* 受保护RF配置事务：共享TX LO和带宽；任一步失败都保持PL/RF停发。 */
static void tx_apply_config(u8 tx1_mode, u8 tx2_mode, u64 lo_hz, u32 bw_hz,
		u32 atten1_mdB, u32 atten2_mdB, u8 line_code, u8 rolloff) {
	u8 stopped = 0U;
#if P351_MANUAL_RF_TEST
	if((tx1_mode || tx2_mode) &&
	   (atten1_mdB-P351_MANUAL_RF_MIN_ATTEN_MDB>89750U-P351_MANUAL_RF_MIN_ATTEN_MDB || atten1_mdB%250U)) {
		xil_printf("[manual-rf] TXCF rejected: invalid attenuation; previous task unchanged\r\n");
		return;
	}
#endif
	/* TXCF会写PLL/滤波并可能进入ALERT；连续RX期间不得打断ADC样本。
	 * TXM紧急停发仍由独立门控处理，不受本换频互斥影响。 */
	if(rx_receiver_active()){
		/* RXQ1保留当前任务状态；这里不能为BUSY打印而阻塞满速接收。 */
		return;
	}
	/* Preserve a previously validated parameter command that arrived before
	 * TXCF. Never reinterpret a cached PCM randomizer under a different code. */
	u8 restore_pcm = pcm_enable_current && line_code == line_code_current;
	u8 restore_soqpsk = soqpsk_enable_current;
	if (!tx_mode_valid(tx1_mode) || !tx_mode_valid(tx2_mode)
			|| tx2_mode != TX_MODE_OFF || tx_modes_mixed(tx1_mode, tx2_mode) || lo_hz < 70000000ULL
			|| lo_hz > 6000000000ULL || bw_hz < AD9361_TX_RF_BW_MIN_HZ
			|| bw_hz > AD9361_TX_RF_BW_MAX_HZ || bw_hz > sample_rate
			|| atten1_mdB > TX_MUTE_ATTEN_MDB || atten2_mdB > TX_MUTE_ATTEN_MDB
			|| line_code > 6U || rolloff > 5U) {
		xil_printf("[tx] rejected invalid TXCF\r\n");
		return;
	}
	if (!refclk_valid) {
		xil_printf("[tx] rejected TXCF: reference clock not verified\r\n");
		return;
	}
	tx_rf_config_inhibited = 1U;
	/* Stop an old active stream, but retain a new task that the UI has already
	 * fully COMMITted and intentionally starts only after this TXCF completes. */
	if (dma_tx_bit_quiesce() != XST_SUCCESS) {
		xil_printf("[tx] BIT quiesce failed; retune aborted\r\n");
		goto rf_config_failed;
	}
	tx_bit_protocol_mode_change();
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS) {
		goto rf_config_failed;
	}
	pcm_enable_current = 0U;
	soqpsk_enable_current = 0U;
	tx_bit_protocol_set_standard_guard(TXB1_GUARD_BLOCKED, 0U);
	tx1_mode_current = 0;
	tx2_mode_current = 0;
	if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, shape_control_word()) ||
			!Axi_Ctrl_Write_Checked(DDS_INC, 0U) ||
			tx_retune_checked(lo_hz, bw_hz) != XST_SUCCESS)
		goto rf_config_failed;
	if (rx_rf_monitor_current
			&& rx_rf_tune_checked(lo_hz, bw_hz, RX_RF_MONITOR_GAIN_DB,
					RX_RF_MONITOR_GAIN_DB) != XST_SUCCESS) {
		/* TX已换频但RX失败：禁止在错误路径再次隐式TX校准/回跳。 */
		rx_rf_monitor_current = 0U;
		(void) rx_rf_tune_checked(rx_lo_freq, rx_bandwidth_current,
				gain_rx1, gain_rx2);
		(void) rx2r_set_source(RX2R_SOURCE_TX_DAC);
		xil_printf("[tx] RX self-monitor tune failed; TX remains OFF\r\n");
		goto rf_config_failed;
	}
	/* 校准期间接收到的STOP不能被本次恢复发射越过；只查看，不消费停发请求。 */
	eth_rx_poll();
	udp_network_service();
	screen_uart0_poll();
	/* UART FIFO最多64字节，一轮32字节预算不足以读完APPLY后紧随的STOP。 */
	screen_uart0_poll();
	if (udp_ctrl_tx_stop_pending() || screen_uart0_stop_pending_get()) {
		stopped = 1U;
		goto rf_config_failed;
	}
	tx_lo_freq = lo_hz;
	bandwidth = bw_hz;
	tx1_atten_mdB = atten1_mdB;
	tx2_atten_mdB = atten2_mdB;
	line_code_current = line_code;
	rolloff_current = rolloff;
	pcm_enable_current = restore_pcm;
	soqpsk_enable_current = restore_soqpsk;
	tx_rf_config_inhibited = 0U;
	tx_rf_retune_failed = 0U;
	tx_apply_modes(tx1_mode, tx2_mode);
	if (tx1_mode_current != tx1_mode || tx2_mode_current != tx2_mode)
		goto rf_config_failed;
	xil_printf("[tx] RF LO=%uMHz BW=%ukHz line=%u rolloff=%u ref=%s/%uHz\r\n",
			(unsigned) (tx_lo_freq / 1000000ULL),
			(unsigned) (bandwidth / 1000UL), (unsigned) line_code_current,
			(unsigned) rolloff_current,
			refclk_source_current ? "external" : "internal",
			(unsigned) refclk_rate_current);
	return;
rf_config_failed:
	tx_rf_config_inhibited = 1U;
	tx_rf_retune_failed = 1U;
	modulation_config_fail_closed();
	(void)ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB, true, true, true);
	screen_uart0_set_status(stopped ? "RF OFF" : "RF FAIL");
}

/* REFC选择的是基准输入：内部40MHz/外部10MHz。AD9361硬件输入始终40MHz。
 * 切换事务不阻塞主循环：先停发，随后独立测参考、锁定，最后回ACK。
 * REFC成功仅表示参考资格通过；恢复RF仍需完整TXCF校准事务。
 */
#define REFCLK_APPLY_PENDING 0xFFFFFFFFU
static u8 refclk_transaction_pending;
static u32 refclk_started_ms;

static u32 p351_clock_now_ms(void)
{
	XTime now;
	XTime_GetTime(&now);
	return (u32)(now / (COUNTS_PER_SECOND / 1000U));
}

static u32 refclk_ad9361_verify(void)
{
	int32_t bb, tx, clk, tx_pd;
	if (!ad9361_phy) return 4U;
	bb = ad9361_spi_read(ad9361_phy->spi, REG_CH_1_OVERFLOW);
	tx = ad9361_spi_read(ad9361_phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
	clk = ad9361_spi_read(ad9361_phy->spi, REG_CLOCK_ENABLE);
	tx_pd = ad9361_spi_read(ad9361_phy->spi, REG_TX_SYNTH_POWER_DOWN_OVERRIDE);
	/* RX射频PLL不参与TX参考资格，保留BBPLL提供LVDS DATA_CLK。
	 * 当前TX已静音，允许TX PLL掉电；真正打开RF前再由TXCF验证锁定。 */
	if (bb < 0 || tx < 0 || clk < 0 || tx_pd < 0 ||
		!(bb & BBPLL_LOCK) || !(clk & XO_BYPASS) ||
		(!(tx & VCO_LOCK) && !(tx_pd & (TX_LO_POWER_DOWN | TX_SYNTH_VCO_POWER_DOWN))))
		return 4U;
	return 0U;
}

static u32 refclk_apply_config(u8 source, u32 rate_hz)
{
	u32 now_ms;
	if (!((source == 0U && rate_hz == 40000000UL) ||
		  (source == 1U && rate_hz == 10000000UL))) return 1U;
	if (refclk_transaction_pending || p351_clock_busy() || rx_receiver_active()) return 3U;
	if (refclk_valid && source == refclk_source_current &&
		p351_clock_ready() && p351_clock_source() == source) {
		if (refclk_ad9361_verify() == 0U) return 0U;
		refclk_valid = 0U;
	}
	/* 先停RF和BIT，再动ADF；失败时不更换基准源、不自动重新发送。 */
	udp_stream_force(0U);
	udp_ctrl_capture_clear();
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS ||
		dma_tx_bit_quiesce() != XST_SUCCESS) return 3U;
	tx_bit_protocol_mode_change();
	modulation_config_fail_closed();
	refclk_valid = 0U;
	tx_rf_config_inhibited = 1U;
	tx_rf_retune_failed = 1U;
	now_ms = p351_clock_now_ms();
	if (p351_clock_begin(source, now_ms) != 0) return 4U;
	refclk_started_ms = now_ms;
	refclk_transaction_pending = 1U;
	xil_printf("[p351-clock] BEGIN source=%u input=%uHz AD9361_REF=40000000Hz R=%u N=%u; RF OFF\r\n",
		(unsigned)source, (unsigned)rate_hz, (unsigned)P351_ADF_R_DIVIDER, (unsigned)P351_ADF_N_DIVIDER);
	return REFCLK_APPLY_PENDING;
}

static void refclk_transaction_service(void)
{
	u32 now_ms = p351_clock_now_ms();
	p351_clock_service(now_ms);
	if (refclk_transaction_pending) {
		if (udp_ctrl_tx_stop_pending() || screen_uart0_stop_pending_get()) {
			p351_clock_abort();
			refclk_transaction_pending = 0U;
			refclk_valid = 0U;
			udp_ctrl_refclk_complete(3U);
			screen_uart0_set_status("RF OFF");
			return;
		}
		if (p351_clock_busy()) return;
		if (p351_clock_ready() && refclk_ad9361_verify() == 0U) {
			refclk_source_current = p351_clock_source();
			refclk_rate_current = refclk_source_current ? 10000000UL : 40000000UL;
			refclk_valid = 1U;
			/* 参考/相位改变后下次完整TXCF必须重新QEC，不能沿用旧LO的收敛缓存。 */
			ad9361_phy->last_tx_quad_cal_freq = 0U;
			refclk_transaction_pending = 0U;
			udp_ctrl_refclk_complete(0U);
			screen_uart0_set_status(refclk_source_current ? "10M LOCK" : "40M INT");
			xil_printf("[p351-clock] READY input=%uHz AD9361_REF=40000000Hz flags=0x%X; RF remains OFF until TXCF\r\n",
				(unsigned)refclk_rate_current, (unsigned)p351_clock_inputs());
			return;
		}
		if (p351_clock_state() != P351_CLOCK_FAULT &&
			(u32)(now_ms - refclk_started_ms) < P351_CLOCK_TIMEOUT_MS + 100U) return;
		p351_clock_abort();
		refclk_transaction_pending = 0U;
		refclk_valid = 0U;
		udp_ctrl_refclk_complete(4U);
		screen_uart0_set_status("REF FAIL");
		xil_printf("[p351-clock] FAIL state=%u flags=0x%X; no silent fallback, RF OFF\r\n",
			(unsigned)p351_clock_state(), (unsigned)p351_clock_inputs());
	} else if (refclk_valid && !p351_clock_ready()) {
		/* PL已在缺参考/失锁时关外部TX路径；PS再关闭芯片TX/LO和BIT任务。
		 * 失锁后即使DLD恢复也不自行重开；须重新REFC及TXCF。 */
		refclk_valid = 0U;
		tx_rf_retune_failed = 1U;
		modulation_config_fail_closed();
		udp_ctrl_refclk_invalidate();
		screen_uart0_set_status("REF LOST");
		xil_printf("[p351-clock] LOST flags=0x%X; RF/LO/BIT OFF; explicit recovery required\r\n",
			(unsigned)p351_clock_inputs());
	}
}

/* PCM_CONTROL: bit0 enable, bit1 one-shot clear, bit2 SOQPSK differential,
 * bit3 IRIG-106 15-stage randomizer.  Registers are static controls in the
 * AXI clock domain.  Keep the
 * clear bit asserted long enough to cross the two-stage synchronizer, then
 * leave only enable asserted for continuous operation. */
static void modulation_config_fail_closed(void) {
	/* A failed write invalidates both shared-modulator states. Otherwise an
	 * old software enable/START guard can claim success while PL is disabled. */
	pcm_enable_current = 0U;
	soqpsk_enable_current = 0U;
	tx_rf_config_inhibited = 1U;
	tx_bit_protocol_set_standard_guard(TXB1_GUARD_BLOCKED, 0U);
	tx_bit_protocol_abort();
	(void) dma_tx_bit_quiesce();
	Axi_Ctrl_Write(PCM_CONTROL, shape_control_word());
	Axi_Ctrl_Write(DDS_INC, 0U);
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS) {
		(void)ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB, true, true, true);
	}
	tx1_mode_current = TX_MODE_OFF;
	tx2_mode_current = TX_MODE_OFF;
	screen_uart0_set_status("MOD FAIL");
}

static void pcm_apply_config(u32 bit_rate_word, u32 deviation_word,
		u32 frequency_offset_word, u16 amplitude, u8 filter_profile,
		u8 randomizer, u8 enable) {
	u32 expected_deviation_word;
	u32 deviation_error;
	u32 control_final;
	u8 expected_randomizer = (line_code_current == 6U) ? 1U : 0U;
	if (enable && !refclk_valid) return; /* 切参考/失锁期间不接受新的BIT使能。 */
	/* 启用时必须有非零数字幅度，防止任务运行却没有调制信号；停发仍允许零参数。 */
	if (filter_profile != 0U || amplitude > 0x7FFFU || randomizer > 1U
			|| (enable
					&& (amplitude == 0U || randomizer != expected_randomizer
							|| line_code_current > 6U
							|| bit_rate_word < IRIG_PCM_MIN_RATE_WORD
							|| bit_rate_word > IRIG_PCM_MAX_RATE_WORD))) {
		xil_printf("[pcm] rejected invalid config profile=%u amplitude=%u\r\n",
				(unsigned) filter_profile, (unsigned) amplitude);
		return;
	}
	expected_deviation_word = (u32) ((((u64) bit_rate_word * 35ULL) + 50ULL)
			/ 100ULL);
	/* UI derives this word directly from Rb/Fs, without integer-Hz rounding.
	 * Retain strict h=0.7 validation; do not loosen it to accept display rounding. */
	deviation_error =
			deviation_word > expected_deviation_word ?
					deviation_word - expected_deviation_word :
					expected_deviation_word - deviation_word;
	if (enable && (deviation_error > 1U || frequency_offset_word != 0U)) {
		xil_printf(
				"[pcm] rejected nonstandard dev=0x%08x expected=0x%08x off=0x%08x\r\n",
				(unsigned) deviation_word, (unsigned) expected_deviation_word,
				(unsigned) frequency_offset_word);
		return;
	}
	/* Canonicalize the accepted +/-1 LSB legacy tolerance before programming PL. */
	if (enable)
		deviation_word = expected_deviation_word;
	/* Integrated Bessel phase uses the shared fractional symbol phase and
	 * deviation/Rb. Register 9 remains the common modulator rate word.
	 * PL serial gain setup takes 64 sample clocks (~1.04 us); the existing
	 * 10 us settling below covers it, with bit_ready backpressure as a guard. */
	if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, shape_control_word())
			|| !Axi_Ctrl_Write_Checked(PCM_DEVIATION, deviation_word)
			|| !Axi_Ctrl_Write_Checked(PCM_FREQUENCY_OFFSET,
					frequency_offset_word)
			|| !Axi_Ctrl_Write_Checked(PCM_AMPLITUDE,
					(u32) amplitude | ((u32) filter_profile << 16))
			|| !Axi_Ctrl_Write_Checked(SOQPSK_BIT_RATE_WORD,
					enable ? bit_rate_word : 0U)) {
		modulation_config_fail_closed();
		xil_printf("[pcm] AXI register readback failed; task aborted and RF OFF\r\n");
		return;
	}
	if (enable) {
		control_final = shape_control_word() | 1U | (randomizer ? 8U : 0U);
		usleep(10);
		if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, control_final | 2U)) {
			modulation_config_fail_closed();
			return;
		}
		usleep(10);
		if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, control_final)) {
			modulation_config_fail_closed();
			return;
		}
	}
	/* Commit software state only after every protected MMIO write succeeds. */
	pcm_bit_rate_word_current = bit_rate_word;
	pcm_deviation_word_current = deviation_word;
	pcm_frequency_offset_word_current = frequency_offset_word;
	pcm_amplitude_current = amplitude;
	pcm_filter_profile_current = filter_profile;
	pcm_randomizer_current = randomizer;
	pcm_enable_current = enable ? 1U : 0U;
	soqpsk_enable_current = 0U;
	if (enable) {
		tx_bit_protocol_set_standard_guard(2U, bit_rate_word);
	} else {
		tx_bit_protocol_set_standard_guard(0U, 0U);
	}
	xil_printf(
			"[pcm] %s rate=0x%08x dev=0x%08x off=0x%08x amp=%u profile=%u line=%u rand=%s\r\n",
			enable ? "enabled" : "disabled", (unsigned) bit_rate_word,
			(unsigned) deviation_word, (unsigned) frequency_offset_word,
			(unsigned) amplitude, (unsigned) filter_profile,
			(unsigned) line_code_current, randomizer ? "IRIG15" : "OFF");
}

/* SOQPSK-TG uses the same offset, amplitude and shared enable/clear control
 * as PCM/FM, plus reg9 for Rb/Fs in Q0.32.  control[2] enables the standard
 * differential encoder before the ternary precoder. */
static void soqpsk_apply_config(u32 bit_rate_word, u32 frequency_offset_word,
		u16 amplitude, u8 differential, u8 randomizer, u8 enable) {
	u32 control_final;
	if (enable && !refclk_valid) return;
	if (amplitude > 0x7FFFU || differential > 1U || randomizer > 1U
			|| (enable && (amplitude == 0U || differential != 1U || randomizer != 1U))
			|| (enable
					&& (bit_rate_word < IRIG_SOQPSK_MIN_RATE_WORD
							|| bit_rate_word > IRIG_SOQPSK_MAX_RATE_WORD
							|| frequency_offset_word != 0U))) {
		xil_printf("[soqpsk] rejected rate=0x%08x amp=%u diff=%u\r\n",
				(unsigned) bit_rate_word, (unsigned) amplitude,
				(unsigned) differential);
		return;
	}
	if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, shape_control_word())
			|| !Axi_Ctrl_Write_Checked(SOQPSK_BIT_RATE_WORD, bit_rate_word)
			|| !Axi_Ctrl_Write_Checked(PCM_FREQUENCY_OFFSET,
					frequency_offset_word)
			|| !Axi_Ctrl_Write_Checked(PCM_AMPLITUDE, (u32) amplitude)) {
		modulation_config_fail_closed();
		xil_printf(
				"[soqpsk] AXI register readback failed; task aborted and RF OFF\r\n");
		return;
	}
	if (enable) {
		control_final = shape_control_word() | 1U | 4U | 8U;
		usleep(10);
		if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, control_final | 2U)) {
			modulation_config_fail_closed();
			return;
		}
		usleep(10);
		if (!Axi_Ctrl_Write_Checked(PCM_CONTROL, control_final)) {
			modulation_config_fail_closed();
			return;
		}
	}
	soqpsk_bit_rate_word_current = bit_rate_word;
	soqpsk_frequency_offset_word_current = frequency_offset_word;
	soqpsk_amplitude_current = amplitude;
	soqpsk_differential_current = differential;
	soqpsk_enable_current = enable ? 1U : 0U;
	pcm_enable_current = 0U;
	if (enable) {
		tx_bit_protocol_set_standard_guard(4U, bit_rate_word);
	} else {
		tx_bit_protocol_set_standard_guard(0U, 0U);
	}
	xil_printf(
			"[soqpsk] %s rate=0x%08x off=0x%08x amp=%u diff=%u rand=IRIG15\r\n",
			enable ? "enabled" : "disabled", (unsigned) bit_rate_word,
			(unsigned) frequency_offset_word, (unsigned) amplitude,
			(unsigned) differential);
}

#include "screen_up70_api.h"
/* 运行时读取芯片而不是只信任PS缓存；调用频率由整机控制器限为10Hz。 */
static u8 screen_up70_chip_healthy(u32 expected_atten)
{
    int32_t gate,pll,lo_pd,bist,att;
    if(!ad9361_phy) return 0U;
    gate=ad9361_spi_read(ad9361_phy->spi,REG_TX_ENABLE_FILTER_CTRL);
    pll=ad9361_spi_read(ad9361_phy->spi,REG_TX_CP_OVERRANGE_VCO_LOCK);
    lo_pd=ad9361_spi_read(ad9361_phy->spi,REG_TX_SYNTH_POWER_DOWN_OVERRIDE);
    bist=ad9361_spi_read(ad9361_phy->spi,REG_BIST_CONFIG);
    att=ad9361_get_tx_atten(ad9361_phy,RF_TX_PHYSICAL_CHANNEL);
    return gate>=0 && pll>=0 && lo_pd>=0 && bist>=0 && !(bist&BIST_ENABLE) && att>=0 &&
        (u32)att==expected_atten && (pll&VCO_LOCK) && !(lo_pd&TX_LO_POWER_DOWN) &&
        ((u8)gate&TX_CHANNEL_ENABLE(TX_1|TX_2))==TX_CHANNEL_ENABLE(RF_TX_PHYSICAL_CHANNEL) &&
        ad9361_ensm_get_state(ad9361_phy)==ENSM_STATE_FDD;
}
/* 前台执行，不从UART中断调用；STOP只观察，不消费，留给主循环完成停发。 */
static u8 screen_stop_requested(void) {
	eth_rx_poll();
	udp_network_service();
	screen_uart0_poll();
	screen_uart0_poll();
	return screen_up70_guard() || udp_ctrl_tx_stop_pending() || screen_uart0_stop_pending_get();
}

/* 屏幕APPLY复用RF事务、标准调制器和本地PN11后端；不依赖UI/UDP下载BIT。
 * 先完整校验，停旧任务并换频，配置调制/新帧，最后START与真实PL进度核验。
 * 返回成功仅代表数字/寄存器链路，不代表RF功率、频谱或解调验收通过。 */
static u32 screen_execute_tx(const screen_apply_t *request, u64 lo_hz,
                            u32 rate, u32 attenuation) {
	pn11_frame_config_t frame;
	u32 rate_word, query = 0U, task_id;
	u32 rf_bandwidth_hz;
	u8 mode, line;
	XTime began, now;
	const char *stage = "parameters";
	/* 可选同步字只属于本次APPLY，不使用“先改全局配置，再应用”的竞态接口。
	 * 旧v1/v2解码将新字段清0，保持FE6B2840/32bit默认及原衰减单位。 */
	if (!request || (request->sync_bits &&
			(request->sync_bits < 16U || request->sync_bits > 33U ||
			 ((((u64)request->sync_word_high << 32) | request->sync_word_low) >> request->sync_bits))) ||
			(!request->sync_bits && (request->sync_word_low || request->sync_word_high)) ||
			request->system_profile > 2U || request->total_atten_halfdb > 126U ||
			(!request->system_profile && request->total_atten_halfdb) || request->mode > 2U ||
			(request->rf_bandwidth_hz && request->rf_bandwidth_hz != 20000000U &&
			 request->rf_bandwidth_hz != 56000000U) ||
			request->framed > 1U || request->frame_count < 1U || request->frame_count > 256U ||
			request->frame_bytes < 1U || request->frame_bytes > 2048U ||
			request->frame_bytes * request->frame_count > PN11_REPLAY_BYTES ||
			(request->mode != 2U && (rate < 100000U || rate > 20000000U ||
			 (request->framed && request->frame_bytes * 8U <=
			  (request->sync_bits ? request->sync_bits : 32U) + (request->frame_count > 1U ? 8U : 0U)))))
		return XST_FAILURE;
	rf_bandwidth_hz = request->rf_bandwidth_hz ? request->rf_bandwidth_hz : 20000000U;
	/* 与UDP TXCF保持同一互斥规则；在停止旧BIT/重写调制器之前拒绝。
	 * 仅在tx_apply_config内返回BUSY不够，之前的步骤已经可能破坏正在运行的TX。 */
	if (rx_receiver_active() || refclk_transaction_pending || p351_clock_busy()) {
		screen_uart0_set_status("BUSY");
		return XST_FAILURE;
	}
	screen_uart0_set_busy(1U);
	screen_task_active = 0U;
	mode = request->mode == 0U ? TX_MODE_PCM_FM :
			(request->mode == 1U ? TX_MODE_SOQPSK_TG : TX_MODE_CW);
	line = request->mode == 0U ? (u8)request->line_code : 0U;
	task_id = 0x53000000U | request->sequence;
	stage = "reference";
	if (!refclk_valid || sample_rate != 61440000U || screen_stop_requested())
		goto fail;
	stage = "quiesce";
	if (dma_tx_bit_quiesce() != XST_SUCCESS)
		goto fail;
	tx_bit_protocol_abort();
	pcm_enable_current = soqpsk_enable_current = 0U;
	stage = "RF_configuration";
	/* 新固定IF事务用56MHz覆盖100k..20M；旧内部接口0保持20MHz。
	 * 20M PCM/FM的Carson粗估约42MHz，20MHz固定滤波有截谱风险。
	 * 56MHz只是滤波配置，不是占用带宽/功率/射频已经验收。 */
	tx_apply_config(TX_MODE_OFF, TX_MODE_OFF,
			lo_hz, rf_bandwidth_hz,
			attenuation, TX_MUTE_ATTEN_MDB, line, 0U);
	if (tx_rf_config_inhibited || tx_rf_retune_failed || !refclk_valid ||
			tx_lo_freq != lo_hz ||
			bandwidth != rf_bandwidth_hz || tx1_atten_mdB != attenuation ||
			line_code_current != line || screen_stop_requested())
		goto fail;
	if (mode != TX_MODE_CW) {
		rate_word = (u32)((((u64)rate << 32) + sample_rate / 2U) / sample_rate);
		pn11_frame_defaults(&frame);
		if (request->sync_bits) {
			frame.sync_bits = (u8)request->sync_bits;
			frame.sync_word = ((u64)request->sync_word_high << 32) | request->sync_word_low;
		}
		frame.framed = (u8)request->framed;
		if (frame.framed) {
			frame.frame_bytes = (u16)request->frame_bytes;
			frame.frame_count = (u16)request->frame_count;
		}
		stage = "local_PN11";
		if (tx_bit_protocol_prepare_local_pn11(&frame, task_id, rate,
				request->infinite ? UINT32_C(0xFFFFFFFF) : 0U,
				mode == TX_MODE_PCM_FM ? 2U : 4U) != TX_LOCAL_OK)
			goto fail;
		stage = "modulator";
		if (mode == TX_MODE_PCM_FM) {
			pcm_apply_config(rate_word, (u32)(((u64)rate_word * 35U + 50U) / 100U),
					0U, SCREEN_MOD_AMPLITUDE, 0U, line == 6U ? 1U : 0U, 1U);
			if (!pcm_enable_current || pcm_bit_rate_word_current != rate_word)
				goto fail;
		} else {
			soqpsk_apply_config(rate_word, 0U, SCREEN_MOD_AMPLITUDE, 1U, 1U, 1U);
			if (!soqpsk_enable_current || soqpsk_bit_rate_word_current != rate_word)
				goto fail;
		}
	}
	stage = "RF_enable";
	if (request->system_profile) {
		u32 output_status;
		/* 上游换频/校准期间外部衰减一直保持63dB；只在最后RF门控前改为目标档。
		 * AXI回读仅证明数字电平，实际插损/校准静默仍要外部硬件验收。 */
		stage = "output_attenuation";
		if(request->system_profile==2U) screen_up70_output_stage(request->total_atten_halfdb);
		output_status = tfs_control_request(4U, 5U, request->total_atten_halfdb);
		if (!(output_status & 0x200U) || (output_status & 0x00ff0000U)) goto fail;
	}
	if (screen_stop_requested()) goto fail;
	if (request->system_profile==2U) {
		stage = "UP70_final_interlock";
		if(!screen_up70_interlock(lo_hz,rate,attenuation,request->total_atten_halfdb)) goto fail;
	} else if (request->system_profile==1U) {
#ifdef TFS_BASE
		p351_tx_plan_t plan;
		u64 signal_khz=lo_hz/1000ULL+(request->mode==2U?2000U:0U);
		u64 final_khz=signal_khz;
		u64 programmed_tfs;
		u32 s,opcode,levels,target=request->total_atten_halfdb;
		/* 最后一次轮询网口以后再读硬件，不在此门控检查与RF开启之间处理UDP。
		 * 旧端可能在长校准/APPLY中写TFS；完成后的校验只能停掉错误RF，不能防止误开。
		 * 这里的寄存器只是PL实际请求，不是TFS芯片应答或RF测频。 */
		stage = "final_RF_interlock";
		if (lo_hz%1000ULL) goto fail;
		if (signal_khz>=2200000U && signal_khz<=2400000U)
			final_khz+=P351_UPCONVERTER_LO_KHZ;
		if (final_khz>UINT32_MAX || !p351_tx_plan(request->mode,(u32)final_khz,&plan) ||
				(u64)plan.ad_lo_khz*1000ULL!=lo_hz || p351_tx_plan_blockers(&plan) ||
				attenuation!=P351_DRIVER_FIXED_ATTEN_MDB) goto fail;
		s=tfs_control_status();
		levels=target>=63U?((~(target-63U))&63U):(64U|((~target)&63U));
		if ((s&0x300U)!=0x300U || (s&0x00ff0005U) || ((s>>24)&127U)!=levels)
			goto fail;
		if (plan.path==P351_PATH_UPCONVERT) {
			programmed_tfs=(u64)Xil_In32(TFS_BASE+0x10U) |
					((u64)Xil_In32(TFS_BASE+0x14U)<<32);
			opcode=Xil_In32(TFS_BASE+0x24U);
			s=tfs_control_status();
			if (!(s&8U) || (s&0x00ff0005U) || opcode!=2U ||
					programmed_tfs!=(u64)plan.tfs_lo_khz*10000ULL) goto fail;
		}
#else
		/* 缺少匹配TFS/TTL外设的旧BSP不得绕过整机联锁；旧profile0不受影响。 */
		goto fail;
#endif
	}
	tx_apply_modes(mode, TX_MODE_OFF);
	if (tx1_mode_current != mode || tx2_mode_current != TX_MODE_OFF ||
			!Axi_Ctrl_Write_Checked(DDS_INC, (u32)mode << TX1_MODE_SHIFT) ||
			screen_stop_requested()) goto fail;
	if (mode == TX_MODE_CW) {
		screen_uart0_set_status("CW ON");
	} else {
		stage = "local_START";
		if (tx_bit_protocol_start_local_pn11(task_id) != TX_LOCAL_OK) goto fail;
		stage = "PL_progress";
		XTime_GetTime(&began);
		do {
			if (screen_stop_requested()) goto fail;
			tx_bit_protocol_service();
			query = tx_bit_protocol_query(task_id);
			if (query & 32U) goto fail;
			if ((query & (4U | 64U)) == (4U | 64U) &&
					(!request->infinite || (query & 16U))) break;
			usleep(100U);
			XTime_GetTime(&now);
		} while (now - began < (COUNTS_PER_SECOND / 1000U) * SCREEN_START_TIMEOUT_MS);
		if ((query & (4U | 64U)) != (4U | 64U) ||
				(request->infinite && !(query & 16U))) goto fail;
		screen_task_id = task_id;
		screen_task_infinite = (u8)request->infinite;
		screen_task_active = 1U;
		screen_uart0_set_status(mode == TX_MODE_PCM_FM ? "PCM RUN" : "SOQ RUN");
	}
	if (screen_stop_requested()) goto fail;
	screen_uart0_set_busy(0U);
	xil_printf("[screen-apply] OK seq=%u TX%u mode=%u LO_kHz=%u att_mdB=%u q=%02x; RF measurement required\r\n",
			(unsigned)request->sequence, (unsigned)RF_TX_PHYSICAL_CHANNEL, (unsigned)mode,
			(unsigned)(lo_hz/1000U), (unsigned)attenuation, (unsigned)query);
	return XST_SUCCESS;
fail:
	screen_task_active = 0U;
	modulation_config_fail_closed();
	if (request->system_profile) (void)tfs_control_request(4U, 5U, 126U);
	screen_uart0_set_busy(0U);
	screen_uart0_set_status("TX FAIL");
	xil_printf("[screen-apply] FAIL seq=%u stage=%s q=%02x; task aborted, RF OFF\r\n",
			(unsigned)request->sequence, stage, (unsigned)query);
	return XST_FAILURE;
}

static u32 screen_apply_request(const screen_apply_t *request) {
    (void)request;
    /* 旧v1没有固定IF/逐调制功率标定语义。必须升级TFT，不能用旧按钮开RF。 */
    screen_uart0_set_status("OLD SCREEN");
    return XST_FAILURE;
}

/* 单次任务完成后停RF；网络换了任务时只放弃屏幕所有权，不停新网络任务。 */
static void screen_task_service(void) {
	u32 query;
	if (!screen_task_active) return;
	query = tx_bit_protocol_query(screen_task_id);
	if (!(query & 64U)) { screen_task_active = 0U; return; }
	if ((query & 32U) || (screen_task_infinite && !(query & (8U | 16U)))) {
		screen_task_active = 0U;
		modulation_config_fail_closed();
		screen_uart0_set_status("BIT FAIL");
	} else if (!screen_task_infinite && (query & 4U) && !(query & (8U | 16U))) {
		screen_task_active = 0U;
		tx_apply_modes(TX_MODE_OFF, TX_MODE_OFF);
		screen_uart0_set_status(tx1_mode_current == TX_MODE_OFF ? "TX DONE" : "OFF FAIL");
	}
}

#include "screen_controller.inc"

#if BOOT_PCM_FM_ENABLED
/* 读的是芯片RFPLL回算频率，不用PS目标缓存代替；失败仍由启动流程统一停发。
 * xil_printf不支持可靠的64位十进制，拆为整数MHz与六位Hz小数，保留1 Hz分辨率。 */
static u32 boot_pcm_fm_verify_lo(const char *stage) {
	u64 lo_read = 0U;
	int32_t read_rc = ad9361_get_tx_lo_freq(ad9361_phy, &lo_read);
	u32 valid = read_rc == XST_SUCCESS && boot_pcm_fm_lo_readback_valid(lo_read);
	xil_printf("[boot-pcm] LO stage=%s target=%u.%06uMHz read=%u.%06uMHz tol_Hz=%u rc=%d %s\r\n",
			stage, (unsigned)(BOOT_PCM_FM_LO_HZ / 1000000U),
			(unsigned)(BOOT_PCM_FM_LO_HZ % 1000000U),
			(unsigned)(lo_read / 1000000U), (unsigned)(lo_read % 1000000U),
			(unsigned)BOOT_PCM_FM_LO_READBACK_TOLERANCE_HZ, (int)read_rc,
			valid ? "PASS" : "FAIL");
	return valid ? XST_SUCCESS : XST_FAILURE;
}

/* 上电自动发射只在配置开关开启时编译，不依赖网口、屏幕或BIST。
 * 必须在AD9361初始化时使用目标LO完成校准，不能绕过运行时大跨度换频保护。 */
static u32 boot_pcm_fm_start(void) {
	pn11_frame_config_t cfg;
	u32 rc, query = 0U, attenuation = 0U;
	u32 rate_word = boot_pcm_fm_rate_word(sample_rate);
	u32 deviation_word = boot_pcm_fm_deviation_word(rate_word);
	int32_t lock, bist;
	XTime began, now;
	const char *stage = "reference";
	if (!refclk_valid || !p351_clock_ready() ||
			refclk_source_current != P351_BOOT_REFERENCE_SOURCE ||
			refclk_rate_current != P351_BOOT_REFERENCE_INPUT_HZ || screen_stop_requested())
		goto fail;
	stage = "initial_LO";
	if (boot_pcm_fm_verify_lo(stage) != XST_SUCCESS)
		goto fail;
	stage = "LVDS_interface";
	if (tx_lvds_port_verify(0U) != XST_SUCCESS)
		goto fail;
	boot_pcm_fm_frame_config(&cfg); /* 所有数据源/组帧参数从唯一配置文件读取。 */
	stage = "local_PN11_prepare";
	rc = tx_bit_protocol_prepare_local_pn11(&cfg, BOOT_PCM_FM_TASK_ID,
			BOOT_PCM_FM_RATE_HZ, BOOT_PCM_FM_REPEAT, 2U);
	if (rc != TX_LOCAL_OK)
		goto fail;
	line_code_current = BOOT_PCM_FM_LINE_CODE;
	rolloff_current = 0U;
	stage = "PCM_registers";
	pcm_apply_config(rate_word, deviation_word, 0U,
			BOOT_PCM_FM_AMPLITUDE, 0U, BOOT_PCM_FM_RANDOMIZER, 1U);
	if (!pcm_enable_current)
		goto fail;
	stage = "RF_configuration";
	if (screen_stop_requested()) goto fail;
	tx_apply_config(TX_MODE_PCM_FM, TX_MODE_OFF, BOOT_PCM_FM_LO_HZ,
			BOOT_PCM_FM_BW_HZ, BOOT_PCM_FM_ATTEN_MDB, TX_MUTE_ATTEN_MDB,
			BOOT_PCM_FM_LINE_CODE, 0U);
	if (tx1_mode_current != TX_MODE_PCM_FM || tx2_mode_current != TX_MODE_OFF ||
			!pcm_enable_current || !refclk_valid ||
			tx_lo_freq != BOOT_PCM_FM_LO_HZ || bandwidth != BOOT_PCM_FM_BW_HZ ||
			line_code_current != BOOT_PCM_FM_LINE_CODE || pcm_randomizer_current != BOOT_PCM_FM_RANDOMIZER)
		goto fail;
	stage = "RF_readback";
	/* TX LO从停发状态唤醒后允许短暂锁定等待，超时/读SPI失败则停发。 */
	XTime_GetTime(&began);
	do {
		if (screen_stop_requested()) goto fail;
		lock = ad9361_spi_read(ad9361_phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
		if (lock < 0)
			goto fail;
		if (lock & VCO_LOCK)
			break;
		usleep(100U);
		XTime_GetTime(&now);
	} while (now - began < (COUNTS_PER_SECOND / 1000U) * BOOT_PCM_FM_START_TIMEOUT_MS);
	/* RF配置及LO唤醒后再次验证真实读回，防止只检查初始值或目标缓存。 */
	stage = "RF_LO_readback";
	if (boot_pcm_fm_verify_lo(stage) != XST_SUCCESS)
		goto fail;
	stage = "RF_readback";
	bist = ad9361_spi_read(ad9361_phy->spi, REG_BIST_CONFIG);
	if (lock < 0 || !(lock & VCO_LOCK) || bist < 0 || (bist & BIST_ENABLE) ||
			ad9361_get_tx_attenuation(ad9361_phy, 0U, &attenuation) != XST_SUCCESS ||
			attenuation != BOOT_PCM_FM_ATTEN_MDB ||
			!Axi_Ctrl_Write_Checked(DDS_INC, (u32)TX_MODE_PCM_FM << TX1_MODE_SHIFT))
		goto fail;
	stage = "local_START";
	if (screen_stop_requested()) goto fail;
	if (tx_bit_protocol_start_local_pn11(BOOT_PCM_FM_TASK_ID) != TX_LOCAL_OK)
		goto fail;
	stage = "PL_progress";
	XTime_GetTime(&began);
	do {
		if (screen_stop_requested()) goto fail;
		tx_bit_protocol_service();
		query = tx_bit_protocol_query(BOOT_PCM_FM_TASK_ID);
		if (screen_stop_requested()) goto fail;
		if (query & 32U)
			goto fail;
		/* 单次短帧可能已经结束：必须验证真实进度，但仅无限重播要求仍active。 */
		if ((query & (4U | 64U)) == (4U | 64U) &&
				(BOOT_PCM_FM_REPEAT != UINT32_C(0xFFFFFFFF) || (query & 16U))) {
			boot_pcm_fm_result = 2U;
			xil_printf("[boot-pcm] RUN LO_kHz=%u rate=%u line=%u rand=%u PN11=%03x framed=%u sync_bits=%u sync_hi=%x sync_lo=%08x minor=%u count=%u repeat=%u\r\n",
				(unsigned)(BOOT_PCM_FM_LO_HZ / 1000U), (unsigned)BOOT_PCM_FM_RATE_HZ,
				(unsigned)BOOT_PCM_FM_LINE_CODE, (unsigned)BOOT_PCM_FM_RANDOMIZER,
				(unsigned)cfg.seed, (unsigned)cfg.framed, (unsigned)cfg.sync_bits,
				(unsigned)(cfg.sync_word >> 32), (unsigned)cfg.sync_word,
				(unsigned)cfg.frame_bytes, (unsigned)cfg.frame_count, (unsigned)BOOT_PCM_FM_REPEAT);
			xil_printf("[boot-pcm] target_mdBm=%d zero_cal_mdBm=%d att_mdB=%u amp=%u ref_source=%u input_Hz=%u AD9361_REF=40000000Hz BW_Hz=%u q=%02x; RF measurement required\r\n",
				(int)BOOT_PCM_FM_TARGET_MDBM, (int)BOOT_PCM_FM_ZERO_MDBM,
				(unsigned)BOOT_PCM_FM_ATTEN_MDB, (unsigned)BOOT_PCM_FM_AMPLITUDE,
				(unsigned)refclk_source_current, (unsigned)refclk_rate_current,
				(unsigned)BOOT_PCM_FM_BW_HZ, (unsigned)query);
			return XST_SUCCESS;
		}
		usleep(100U);
		XTime_GetTime(&now);
	} while (now - began < (COUNTS_PER_SECOND / 1000U) * BOOT_PCM_FM_START_TIMEOUT_MS);
fail:
	boot_pcm_fm_result = 3U;
	modulation_config_fail_closed();
	xil_printf("[boot-pcm] FAIL stage=%s q=0x%02x; task aborted, RF OFF, no automatic retry\r\n",
			stage, (unsigned)query);
	return XST_FAILURE;
}
#endif

/* 默认参考只发起一次；放在网口/屏幕初始化后，避免PHY耗时耗尽ADF锁定时限。
 * 此前refclk_valid始终为0，RF保持关闭；主循环在处理新控制请求前推进事务。 */
static void boot_reference_init(void)
{
	u32 result;
	refclk_valid = 0U;
	boot_reference_pending = 1U;
	result = refclk_apply_config(P351_BOOT_REFERENCE_SOURCE, P351_BOOT_REFERENCE_INPUT_HZ);
	if (result != REFCLK_APPLY_PENDING) {
		boot_reference_pending = 0U;
#if BOOT_PCM_FM_ENABLED
		boot_pcm_fm_result = 3U;
#endif
		modulation_config_fail_closed();
		xil_printf("[boot-ref] FAIL begin=%u; RF OFF, no fallback/retry\r\n", (unsigned)result);
	} else {
		xil_printf("[boot-ref] WAIT source=%u input_Hz=%u AD9361_REF=40000000Hz; RF OFF\r\n",
			(unsigned)P351_BOOT_REFERENCE_SOURCE, (unsigned)P351_BOOT_REFERENCE_INPUT_HZ);
	}
}

static void boot_reference_service(void)
{
	if (!boot_reference_pending) return;
	if (screen_stop_requested()) {
		/* 即使之后参考恢复/人工切源，也不再次启动本次上电自动TX任务。 */
		boot_reference_pending = 0U;
#if BOOT_PCM_FM_ENABLED
		boot_pcm_fm_result = 3U;
#endif
		modulation_config_fail_closed();
		xil_printf("[boot-ref] CANCEL STOP; RF OFF, automatic TX cancelled\r\n");
		return;
	}
	if (refclk_transaction_pending || p351_clock_busy()) return;
	boot_reference_pending = 0U; /* 先清标志，成功/失败都只允许一次启动尝试。 */
	if (!refclk_valid || !p351_clock_ready() ||
			refclk_source_current != P351_BOOT_REFERENCE_SOURCE ||
			refclk_rate_current != P351_BOOT_REFERENCE_INPUT_HZ) {
#if BOOT_PCM_FM_ENABLED
		boot_pcm_fm_result = 3U;
#endif
		modulation_config_fail_closed();
		xil_printf("[boot-ref] FAIL qualification; RF OFF, no fallback/retry\r\n");
		return;
	}
#if BOOT_PCM_FM_ENABLED
	(void)boot_pcm_fm_start();
#else
	xil_printf("[boot-ref] READY source=%u input_Hz=%u AD9361_REF=40000000Hz; auto_tx=0, RF remains OFF\r\n",
		(unsigned)refclk_source_current, (unsigned)refclk_rate_current);
#endif
}

/* ==================== main ==================== */
int main(void) {
	int32_t init_status;
	u32 rx_rate = 0U, tx_rate = 0U;
	Xil_ICacheEnable();
	Xil_DCacheEnable();

	Xil_AssertSetCallback(dbg_assert_cb); /* 避免断言静默卡死，保留定位信息。 */

	xil_printf(
			"\r\n=== AD9361TX 1R1T LVDS (61.44 MSPS / 122.88 MHz DATA_CLK) ===\r\n");
	xil_printf("[rf-channel] profile RX%uA/TX%uA only; logical slot0 active, slot1 disabled\r\n",
			(unsigned)RF_RX_PHYSICAL_CHANNEL, (unsigned)RF_TX_PHYSICAL_CHANNEL);
	print_boot_diagnostics();
	tx_lo_freq = BOOT_INITIAL_TX_LO_HZ;
	xil_printf("[boot-config] auto_tx=%u initial_TX_LO_kHz=%u; BIT/UDP service enabled independently\r\n",
			(unsigned)BOOT_PCM_FM_ENABLED, (unsigned)(BOOT_INITIAL_TX_LO_HZ / 1000U));
#if BOOT_PCM_FM_ENABLED
	xil_printf("[boot-pcm] AUTO-TX TEST initial_LO_kHz=%u; no UI/UDP trigger required\r\n",
			(unsigned)(BOOT_INITIAL_TX_LO_HZ / 1000U));
#endif

	/* AD9361硬件初始化：GPIO、SPI，然后按初始LO完成芯片校准。 */
	default_init_param.gpio_resetb = GPIO_RESET_PIN;
	default_init_param.gpio_sync = -1;
	default_init_param.gpio_cal_sw1 = -1;
	default_init_param.gpio_cal_sw2 = -1;

	gpio_init(GPIO_DEVICE_ID); /* EMIO GPIO */
	gpio_direction(default_init_param.gpio_resetb, 1);
	Gpio_Emio_Init(); /* AD9361控制信号方向和初始电平。 */
	if (p351_board_init() != 0)
		tx_boot_fail("P351_board_identity_route", -1);
	init_status = spi_init(SPI_DEVICE_ID, 1, 0); /* AD9361寄存器SPI；与PL上的TFS SPI不同。 */
	if (init_status != SUCCESS)
		tx_boot_fail("AD9361_SPI_init", init_status);

	init_status = ad9361_init(&ad9361_phy, &default_init_param);
	if (init_status != 0 || !ad9361_phy)
		tx_boot_fail("ad9361_init", init_status);
	if (rf_channel_profile_verify() != XST_SUCCESS)
		tx_boot_fail("RF_channel_profile", -5);

	/* 1R1T LVDS operation.  The AD9361 FIR is left bypassed at
	 * 61.44 MSPS; PCM/FM and SOQPSK pulse shaping is implemented in PL.
	 * Init parameters already set both sample rates, LO and RF bandwidth.
	 * Reprogramming that whole clock/RF chain here obscured boot failures. */
	init_status = ad9361_set_trx_rate_gov(ad9361_phy, 0);
	if (init_status != 0)
		tx_boot_fail("rate_governor", init_status);
	init_status = ad9361_set_trx_fir_en_dis(ad9361_phy, DISABLE);
	if (init_status != 0)
		tx_boot_fail("FIR_bypass", init_status);
	init_status = ad9361_get_rx_sampling_freq(ad9361_phy, &rx_rate);
	if (init_status != 0)
		tx_boot_fail("RX_sample_readback", init_status);
	init_status = ad9361_get_tx_sampling_freq(ad9361_phy, &tx_rate);
	if (init_status != 0)
		tx_boot_fail("TX_sample_readback", init_status);
	if (rx_rate < sample_rate - 2U || rx_rate > sample_rate + 2U
			|| tx_rate < sample_rate - 2U || tx_rate > sample_rate + 2U)
		tx_boot_fail("sample_rate_mismatch", -2);
	xil_printf("[main] AD9361 clock-chain RXfs=%u TXfs=%u Hz\r\n",
			(unsigned) rx_rate, (unsigned) tx_rate);
	init_status = ad9361_get_rx_lo_freq(ad9361_phy, &rx_lo_applied_hz);
	if (init_status != 0)
		tx_boot_fail("RX_LO_readback", init_status);
	init_status = ad9361_get_rx_rf_bandwidth(ad9361_phy, &rx_bw_applied_hz);
	if (init_status != 0)
		tx_boot_fail("RX_BW_readback", init_status);

	/* ch=0映射到物理RX2；保护衰减用低层API一次设置两个物理TX。 */
	init_status = ad9361_set_rx_rf_gain(ad9361_phy, 0, gain_rx1);
	if (init_status != 0)
		tx_boot_fail("RX_gain_apply", init_status);
	init_status = ad9361_set_tx_atten(ad9361_phy, TX_MUTE_ATTEN_MDB, true, true, true);
	if (init_status != 0)
		tx_boot_fail("TX_mute_attenuation", init_status);

	/* Calibration temporarily uses FDD.  After the eye scan, the real RF gate
	 * below returns ENSM to RX and powers down the managed TX chain. */
	ad9361_set_en_state_machine_mode(ad9361_phy, ENSM_MODE_FDD);
	Axi_Ctrl_Write(DDS_INC, 0); /* Safe boot state: TX1/TX2 waveform OFF. */
	pcm_apply_config(0U, 0U, 0U, 0U, 0U, 0U, 0U);
	soqpsk_apply_config(0U, 0U, 0U, 1U, 0U, 0U);
	xil_printf("[main] ENSM->FDD for LVDS calibration state=%u\r\n",
			(unsigned) ad9361_ensm_get_state(ad9361_phy));

	/* BIST PN9 eye scan: AD9361 16x16 coarse delays followed by common and
	 * per-lane FPGA IDELAY scans.  Normal RX data is restored before startup
	 * continues; failure keeps the saved interface delays and is reported. */
	if (lvds_rx_auto_calibrate(ad9361_phy, &lvds_cal_result, 1) != 0)
		xil_printf("[main] WARNING: LVDS PRBS calibration failed\r\n");
	if (tx_rf_gate_apply(0U, 0U) != XST_SUCCESS)
		tx_boot_fail("TX_RF_gate", -3);
	if (tx_lvds_port_verify(1U) != XST_SUCCESS)
		tx_boot_fail("TX_LVDS_port", -4);

	/* 仅回读唯一逻辑通道；ch=1不是被禁用的物理RX1。 */
	{
		int32_t g0 = -1;
		init_status = ad9361_get_rx_rf_gain(ad9361_phy, 0, &g0);
		if (init_status != 0 || g0 != gain_rx1)
			tx_boot_fail("RX_gain_readback", init_status ? init_status : -6);
		xil_printf("[main] gain readback RX%u(logical0)=%ddB target=%ddB; other RX disabled\r\n",
				(unsigned) RF_RX_PHYSICAL_CHANNEL, g0, gain_rx1);
	}

	/* 默认保持真实RF接收；数字回环必须由LBCK显式开启，不能冒充SMA回环。 */

	/* xil_printf不支持%llu，LO先换算为MHz再打印，避免格式引起参数错位。 */
	xil_printf(
			"[main] AD9361 1R1T RX%u/TX%u init done: fs=%uHz BW=%uHz RXLO=%uMHz gain=%ddB ensm=%u\r\n",
			(unsigned) RF_RX_PHYSICAL_CHANNEL, (unsigned) RF_TX_PHYSICAL_CHANNEL,
			(unsigned) sample_rate, (unsigned) bandwidth,
			(unsigned) (rx_lo_freq / 1000000UL), gain_rx1,
			(unsigned) ad9361_ensm_get_state(ad9361_phy));

	/* 先安装DMA/GIC，再初始化BIT协议；自动发射只能在这些步骤之后启动。 */
	if (dma_rx_init() != XST_SUCCESS) {
		xil_printf(
				"[main] monitor DMA unavailable; continuing TX/network diagnostics\r\n");
	}
	if (dma_rx_gic_init() != XST_SUCCESS) {
		while (1)
			;
	}
	dma_rx_intr_enable();
	if (dma_tx_bit_init() != XST_SUCCESS) {
		xil_printf("[txbit] init failed; RF remains muted\r\n");
		while (1) {
		}
	}
	tx_bit_protocol_init();
	/* 接收PL可能在PS重新RUN ELF前已运行；先确认新ABI并停止旧解调。
	 * 这一步不调谐RX、不开启TX；RX默认参数从rx_receiver_profile.h读取。 */
	{
		u32 rx_init_status=rx_receiver_init();
		xil_printf("[rx-config] V%u ABI=0x%08x init=%u; RX2 decode OFF, explicit apply required\r\n",
			(unsigned)BOARD_CONFIG_VERSION,(unsigned)RXR_ABI,(unsigned)rx_init_status);
	}

	/* GEM复用已有GIC；网口失败仍继续运行默认参考与本地服务。
	 * refclk_valid=0：初始化通信时也不允许任何任务提前开RF。 */
	udp_net_init();
	/* 配套P351 BSP已启用UART0-EMIO：独立处理屏幕握手、只读快照和原子APPLY。
	 * UART1仅供调试；未带UART0的错误BSP会被构建检查拒绝，不能称为屏幕可用。 */
	screen_uart0_init();
	boot_reference_init();

	/* 网口与独立屏幕共用服务循环；UART字节只排队，业务互斥与STOP在此处理。 */
	xil_printf(
			"[tx-health] 5s compact status; TXST gives full on-demand details; RF registers are not RF measurements\r\n");
	{
		u32 diag_tick = 0; /* 跨迭代保留的诊断限流计数。 */
		u32 hb_cnt = 0; /* 主循环周期计数，不是实际秒数。 */
		while (1) {
			 u8 tx1_mode, tx2_mode, stream_on, loopback_mode, monitor_source;
			 u8 rf_rx_enable;
			u64 cfg_lo_hz;
			u32 cfg_bw_hz, cfg_att1_mdB, cfg_att2_mdB;
			u8 cfg_line_code, cfg_rolloff;
			u8 refclk_source;
			u32 refclk_rate_hz;
			u32 pcm_rate_word, pcm_dev_word, pcm_offset_word;
			u32 soqpsk_rate_word, soqpsk_offset_word;
			u16 pcm_amp;
			u16 soqpsk_amp;
			u8 pcm_profile, pcm_randomizer, pcm_enable;
			u8 soqpsk_differential, soqpsk_randomizer, soqpsk_enable;
			screen_apply_t screen_request;
			screen_command_t pc_final_command;
			u8 final_stop_turn=0U;
			/* ISR只搬运数据/重挂BD；UDP解析和CFGQ快照在主循环，避免长期占用IRQ。 */
			refclk_transaction_service(); /* 参考失锁优先于新的发射请求。 */
			p351_rf_safety_service(); /* 50ms心跳；PL在250ms无进度时独立切断RF。 */
			if(p351_rf_safety_fault() && (tx1_mode_current!=TX_MODE_OFF ||
					tx2_mode_current!=TX_MODE_OFF || board_final_rf_owned())) {
				final_stop_turn=1U;
				if(board_final_rf_owned()) screen_up70_shutdown(10U);
				screen_v2_cancel();
				modulation_config_fail_closed();
				xil_printf("[p351-safety] latched PL interlock; RF OFF; fresh APPLY required\r\n");
			}
			eth_rx_poll();
			udp_network_service();
			/* 只为屏幕拥有的任务启用断屏停发，不干扰独立PC调试任务。
			 * 常规sendme探测2s一次，连续6s无页面回执即STOP；重启报文立即STOP。 */
			screen_uart0_rf_watchdog(board_screen_rf_owned());
			screen_uart0_poll();
			boot_reference_service(); /* 新控制请求之前确认默认参考/一次性启动条件。 */
			if (udp_ctrl_refclk_config_get(&refclk_source, &refclk_rate_hz)) {
				u32 result = refclk_apply_config(refclk_source, refclk_rate_hz);
				if (result != REFCLK_APPLY_PENDING) udp_ctrl_refclk_complete(result);
			}
			if (udp_ctrl_tx_config_get(&tx1_mode, &tx2_mode, &cfg_lo_hz,
					&cfg_bw_hz, &cfg_att1_mdB, &cfg_att2_mdB, &cfg_line_code,
					&cfg_rolloff))
				tx_apply_config(tx1_mode, tx2_mode, cfg_lo_hz, cfg_bw_hz,
						cfg_att1_mdB, cfg_att2_mdB, cfg_line_code, cfg_rolloff);
			if (udp_ctrl_tx_mode_get(&tx1_mode, &tx2_mode)) {
				if(tx1_mode==TX_MODE_OFF && tx2_mode==TX_MODE_OFF) final_stop_turn=1U;
				tx_apply_modes(tx1_mode, tx2_mode);
			}
			if (udp_ctrl_pcm_config_get(&pcm_rate_word, &pcm_dev_word,
					&pcm_offset_word, &pcm_amp, &pcm_profile, &pcm_randomizer,
					&pcm_enable))
				pcm_apply_config(pcm_rate_word, pcm_dev_word, pcm_offset_word,
						pcm_amp,
						/* Do not discard an enable packet if it arrives one main-loop
						 * iteration before TXCF.  pcm_enable is additionally qualified by
						 * pcm_selected inside the PL, so storing it while the current mode is
						 * OFF is safe; the subsequent mode write can then start PCM/FM. */
						pcm_profile, pcm_randomizer, pcm_enable);
			if (udp_ctrl_soqpsk_config_get(&soqpsk_rate_word,
					&soqpsk_offset_word, &soqpsk_amp, &soqpsk_differential,
					&soqpsk_randomizer, &soqpsk_enable))
				soqpsk_apply_config(soqpsk_rate_word, soqpsk_offset_word,
						soqpsk_amp, soqpsk_differential, soqpsk_randomizer,
						soqpsk_enable);
			if (udp_ctrl_loopback_get(&loopback_mode))
				tx_rx_loopback_apply(loopback_mode);
			if (udp_ctrl_rf_rx_get(&rf_rx_enable))
				rx_rf_monitor_apply(rf_rx_enable);
			if (udp_ctrl_monitor_source_get(&monitor_source))
				rx2r_set_source((u32) monitor_source);
			if (udp_ctrl_stream_get(&stream_on))
				udp_stream_force(stream_on);
			tx_bit_protocol_service();
			/* STOP优先；实际APPLY由参数包执行，旧控件ID默认禁用。 */
			if (screen_uart0_take_stop()) {
				final_stop_turn=1U;
				screen_v2_cancel();
				screen_task_active = 0U;
				(void) screen_uart0_take_cw();
				xil_printf("[screen] RF OFF requested\r\n");
				tx_apply_modes(TX_MODE_OFF, TX_MODE_OFF);
				screen_uart0_set_status(
						(tx1_mode_current == TX_MODE_OFF &&
						 tx2_mode_current == TX_MODE_OFF) ? "RF OFF" : "OFF FAIL");
			} else if (screen_uart0_take_apply(&screen_request)) {
				(void)screen_apply_request(&screen_request);
			} else if (screen_uart0_take_cw()) {
				xil_printf("[screen] CW TEST confirmed\r\n");
				tx_apply_modes(TX_MODE_CW, TX_MODE_OFF);
				screen_uart0_set_status(
						(tx1_mode_current == TX_MODE_CW &&
						 tx2_mode_current == TX_MODE_OFF) ? "CW ON" : "CW FAIL");
			}
			screen_task_service();
			if(final_stop_turn) {
				/* 同一轮STOP/PL故障优先：丢弃此前排队的屏幕APPLY，禁止服务尾部复发。 */
				screen_v2_cancel();
				if(screen_uart0_take_command(&pc_final_command))
					screen_uart0_ack(pc_final_command.sequence,pc_final_command.op,SC_STOPPED);
			} else screen_v2_service();
			if(udp_ctrl_final_rf_get(&pc_final_command)) {
				u32 pc_rc=final_stop_turn?SC_STOPPED:board_pc_final_rf_execute(&pc_final_command);
				udp_ctrl_final_rf_complete(pc_final_command.sequence,pc_final_command.op,pc_rc);
			}
			screen_v2_publish();
			tx_status_service((u8)udp_ctrl_tx_status_get());
			board_tx_bist_diag_service();
			if(rx_receiver_active())rx_receiver_service();
			else if(!board_final_rf_owned()) udp_stream_service(); /* 整机联锁运行中不进入阻塞大块上传。 */
			tx_soak_service(); /* Timer based 5 s; nonblocking UART drain. */
			/* 无串口打印的周期计数，实际健康日志按硬件计时器限流。 */
			hb_cnt++;
			/* 依据循环计数重试/刷新ARP；不是严格2s/8s定时。板端配置应答直接返回请求源。 */
			if ((hb_cnt & 0x1FFFFFu) == 0 && !udp_peer_ok())
				udp_arp_resolve();
			else if ((hb_cnt & 0x7FFFFFu) == 0)
				udp_arp_resolve();
			/* Quiet GEM error-bit maintenance. Counters appear in TXST/tx-health. */
			if ((++diag_tick & 0xFFF) == 0) {
				eth_diag_poll();
			}
		}
	}

	Xil_DCacheDisable();
	Xil_ICacheDisable();
	return 0;
}


/* 网络初始化失败不应阻止本地发射；CFGQ只有收到查询时才回传。 */
static void udp_net_init(void) {
	eth_link_init(); /* GEM0、PHY、BD环与中断；复用DMA的GIC。 */
	udp_stack_init();
	udp_stream_init();
	if (eth_link_is_up()) {
		u8 ar = udp_arp_resolve(); /* 初次ARP解析；未成功时主循环继续重试。 */
		xil_printf("[udp] boot ARP %s\r\n", ar == 0 ? "sent" : "fail");
	} else
		xil_printf("[udp] link down, ARP deferred (retry in service)\r\n");
}

/* ==================== GPIO/EMIO ==================== */
void Gpio_Emio_Init(void) {
	/* P351时钟与射频开关统一由p351_board_init初始化，不沿用Z7控制脚。 */
	gpio_direction(GPIO_REF_CAPABILITY, 0);
	gpio_direction(GPIO_TXNRX_PIN, 1);
	gpio_direction(GPIO_ENABLE_PIN, 1);
	gpio_direction(GPIO_RESET_PIN, 1);
	gpio_direction(GPIO_EN_AGC, 1);
	gpio_direction(GPIO_CTL0_PIN, 1);
	gpio_direction(GPIO_CTL1_PIN, 1);
	gpio_direction(GPIO_CTL2_PIN, 1);
	gpio_direction(GPIO_CTL3_PIN, 1);
	gpio_set_value(GPIO_TXNRX_PIN, 1);
	gpio_set_value(GPIO_ENABLE_PIN, 0);
	gpio_set_value(GPIO_RESET_PIN, 1);
	gpio_set_value(GPIO_EN_AGC, 1);
	gpio_set_value(GPIO_CTL0_PIN, 0);
	gpio_set_value(GPIO_CTL1_PIN, 0);
	gpio_set_value(GPIO_CTL2_PIN, 0);
	gpio_set_value(GPIO_CTL3_PIN, 0);
}

/* AXI-Lite基础写接口；关键调制寄存器应使用下方写入后回读校验版本。 */
void Axi_Ctrl_Write(u32 offset, u32 data) {
	Xil_Out32(AXI_CTRL_BASE_ADDR + offset, data);
}

static u8 Axi_Ctrl_Write_Checked(u32 offset, u32 data) {
	u32 readback;
	Xil_Out32(AXI_CTRL_BASE_ADDR + offset, data);
	readback = Xil_In32(AXI_CTRL_BASE_ADDR + offset);
	if (readback != data) {
		xil_printf("[axi] verify fail off=0x%02x wr=0x%08x rd=0x%08x\r\n",
				(unsigned) offset, (unsigned) data, (unsigned) readback);
		return 0U;
	}
	return 1U;
}
