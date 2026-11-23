/*
 * rtos_config.h: FreeRTOS settings for rtos_usb_cdc
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tick hook checks the USB controller once a tick for events it wrote
 * without raising its interrupt.
 */

#define configUSE_TICK_HOOK    1
