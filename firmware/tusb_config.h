// SPDX-License-Identifier: Apache-2.0
#pragma once
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC 1
#ifdef EPHEMERIS_CLOCKED_USB
// Keep several full-speed OUT packets in flight while the other core tracks.
#define CFG_TUD_CDC_EP_BUFSIZE 512
#define CFG_TUD_CDC_RX_BUFSIZE 1024
#else
#define CFG_TUD_CDC_RX_BUFSIZE 128
#endif
#define CFG_TUD_CDC_TX_BUFSIZE 512
