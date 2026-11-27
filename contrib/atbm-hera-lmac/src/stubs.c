/*
 * lmac_qemu: the rest of what the lower MAC exported, as no-ops
 *
 * The application, AT commands and HMAC call these to tune the radio,
 * its rates and its power save, none of which the QEMU model has; each
 * does nothing and returns 0. The variables are the LMAC's settings that
 * HMAC and the AT commands read or write, with the SDK defaults.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define NOOP(name) int name(void); int name(void) { return 0; }
NOOP(atbm_ewma_reset)
NOOP(atbm_wifi_bandedge_power_control_enable)
NOOP(atbm_wifi_check_and_reset_rx_fifo)
NOOP(atbmwifi_reset_tx_status_update)
NOOP(atbmwifi_succ_pkt_cnt_per_sec)
NOOP(CalRssiAgc_Restart)
NOOP(CmdLine_DelayPowerSave)
NOOP(CmdLine_Get_Etf_Rx_Info)
NOOP(CmdLine_Get_TxRx_Stat)
NOOP(CmdLine_GetPlatformVar)
NOOP(CmdLine_Monitor_Cnt_Reset)
NOOP(CmdLine_StateDump)
NOOP(Lmac_ResetRetryTxLiveTime)
NOOP(lmac_Rx_Buffer_Full)
NOOP(LMAC_SetBcnListenIntvl)
NOOP(LMAC_SetCtsToSelf_Enable)
NOOP(Lmac_SetNullDataRetry)
NOOP(Lmac_SetRetryTxLiveTime)
NOOP(LMAC_SetRTSThreshold)
NOOP(lmac_Wakeup)
NOOP(LMC_FM_GetATBMIe)
NOOP(LMC_Get_CCA_IdleRatio)
NOOP(LMC_SetAPKeepAliveEnable)
NOOP(LMC_SetBcnLostCntAndItvlForNullDataSend)
NOOP(LMC_SetSpecRateTxpowerMode)
NOOP(LMC_SetTxHighDutyIdleRatioEnable)
NOOP(PAC_DisablePacPhyIf)
NOOP(PHY_Agc_Enable)
NOOP(PHY_get_cca_and_idle_api)
NOOP(PHY_Initial_DigScaler_Table)
NOOP(PHY_Initial_Ppa_Table)
NOOP(PHY_Manual_control_cfo_dcxo_cali)
NOOP(PHY_ResetRFIP)
NOOP(PHY_RF_Get_Freq_value)
NOOP(PHY_RF_Get_temprature)
NOOP(PHY_RF_set_Delta_DCXO_table)
NOOP(PHY_RF_set_Delta_temp_table)
NOOP(PHY_RF_Set_Freq_value)
NOOP(PHY_RF_SetDeltaGain)
NOOP(PHY_RF_SetDeltaGainReplaceEfuseGain)
NOOP(PHY_RF_SetDigitalGain)
NOOP(PHY_RF_TPC_Cal)
NOOP(PHY_RF_TX_WriteToPPATbl)
NOOP(PHY_Set_adaptive)
NOOP(PHY_Set_agc_flag)
NOOP(PHY_Set_cca_and_target_power_api)
NOOP(PHY_set_txpowerflag)
NOOP(PHY_Update_PA_External)
NOOP(PS_Change_BeaconRecvTimeout)
NOOP(PS_Change_LongWaitBeaconLostCnt)
NOOP(PS_Change_PsLevel)
NOOP(PS_PmDtimWakeupTimerHandler)
NOOP(RX_InitDefragBuffers)
NOOP(SET_LMAC_SLEEP_MODE)
NOOP(SET_NOT_SUPPORT_AMPDU_FLAG)
NOOP(Smart_config_enable)
NOOP(SYS_PowerSuspend)

uint8_t default6441_debug_print;
uint32_t etf_monitor_reduce_power_consumption = 1;
uint8_t g_check_tx_itvl_over_time_limit;
uint8_t g_enable_FEM;
uint8_t g_ignore_broadcast_tim_enable;
uint32_t g_null_rate_customer_set = 0xff;
uint8_t g_rts_long_duration_flag;
uint16_t g_rts_long_duration_value = 0x1f3f;
uint32_t MIB_Flags;
uint32_t ps_debug_flag;
uint32_t ps_debug_rx_bcn_hw_time;
uint32_t ps_debug_rx_bcn_hwdelay_time;
uint32_t ps_dtim_wake_up_flag;
uint32_t real_mode_reduce_power_consumption;
uint8_t support_mgmt_frame_cb;
uint16_t TX_POWER;
/* the frame monitor counters of the ETF test mode */
uint32_t sPasMonitor_etf[6];
