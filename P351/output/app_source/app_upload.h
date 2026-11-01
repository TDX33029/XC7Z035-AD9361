/* RX DMA capture and UDP/40001 upload service. Rail/EMIO triggering removed. */
#ifndef APP_UPLOAD_H
#define APP_UPLOAD_H

#include "xil_types.h"

#define UDP_CAPTURE_GROUPS  131072U
#define UDP_MONITOR_GROUPS  4096U /* SNM1：短帧显示；SNAP长块采集保持不变。 */
#define UDP_CAPTURE_WORDS   UDP_CAPTURE_GROUPS /* SINGLE_SLOT=1 */
#define UDP_N_G_PER_FRAME   160U
#define UDP_PACE_US         500U
#define UDP_STATUS_EVERY    1000U

void udp_stream_init(void);
void udp_stream_service(void);
void udp_stream_force(u8 on);
u8   udp_stream_active(void);
u32  udp_stream_captures(void);
u32  udp_stream_errors(void);
void udp_stream_iq_print(void); /* TXST显式查询最近一次真实DMA样本，不自动采集/发射。 */

#endif
