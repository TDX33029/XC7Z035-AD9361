/******************************************************************************
* Copyright (C) 2010 - 2021 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
*
* @file xaxidma_g.c
* @addtogroup axidma_v9_13
* @{
*
* Provide a template for user to define their own hardware settings.
*
* If using XPS, then XPS will automatically generate this file.
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who  Date     Changes
* ----- ---- -------- -------------------------------------------------------
* 1.00a jz   08/16/10 First release
* 2.00a jz   08/10/10 Second release, added in xaxidma_g.c, xaxidma_sinit.c,
*                     updated tcl file, added xaxidma_porting_guide.h
* 3.00a jz   11/22/10 Support IP core parameters change
* 4.00a rkv  02/22/11 Added support for simple DMA mode
* 6.00a srt  01/24/12 Added support for Multi-Channel DMA mode
* 7.02a srt  01/23/13 Replaced *_TDATA_WIDTH parameters to *_DATA_WIDTH
*		      (CR 691867)
* 9.7   rsp  04/25/18 In XAxiDma_Config add SG length width.
* 9.8   rsp  07/18/18 Sync XAxiDma_Config initializer fields
*
* </pre>
*
******************************************************************************/

/***************************** Include Files *********************************/

#include "xparameters.h"
#include "xaxidma.h"

/************************** Constant Definitions *****************************/
#define XPAR_AXIDMA_0_INCLUDE_SG	0

/* 本板 BOOT.BIN 中的位流与原厂 v3 逐字节一致，AXI DMA 的硬件参数必须按
 * 原厂 BSP 的表填写：RX=0x40400000(S2MM)，TX=0x40410000(MM2S)，
 * burst=16，SG length width=26。HSI 2021.2 从重建 XSA 生成的 64/14 与
 * 实际位流不符；TX 实例还完全缺失，导致 XAxiDma_LookupConfig(1) 返回 NULL，
 * dma_tx_bit_init() 直接失败。 */
XAxiDma_Config XAxiDma_ConfigTable[] =
{
	{
		0U,			/* DeviceId: RX */
		0x40400000U,		/* BaseAddr */
		0,			/* SgIncludeStsCntrlStrm */
		0,			/* IncludeMm2s */
		0,			/* IncludeMm2sDre */
		32,			/* Mm2sDataWidth */
		1,			/* IncludeS2mm */
		0,			/* IncludeS2mmDre */
		64,			/* S2mmDataWidth */
		0,			/* IncludeSg */
		1,			/* NumMm2sChannels */
		1,			/* NumS2mmChannels */
		16U,			/* Mm2sBurstSize */
		16U,			/* S2mmBurstSize */
		0,			/* MicroDmaMode */
		32,			/* AddrWidth */
		26			/* SgLengthWidth */
	},
	{
		1U,			/* DeviceId: TX */
		0x40410000U,		/* BaseAddr */
		0,			/* SgIncludeStsCntrlStrm */
		1,			/* IncludeMm2s */
		0,			/* IncludeMm2sDre */
		64,			/* Mm2sDataWidth */
		0,			/* IncludeS2mm */
		0,			/* IncludeS2mmDre */
		32,			/* S2mmDataWidth */
		0,			/* IncludeSg */
		1,			/* NumMm2sChannels */
		1,			/* NumS2mmChannels */
		16U,			/* Mm2sBurstSize */
		16U,			/* S2mmBurstSize */
		0,			/* MicroDmaMode */
		32,			/* AddrWidth */
		26			/* SgLengthWidth */
	}
};
/** @} */
