// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

/* Peripheral Memory */
#define MT8167_GPIO_BASE                                    0x10005000
#define MT8167_GPIO_SIZE                                    0x700

#define MT8167_SOC_BASE                                     0x10200000
#define MT8167_SOC_SIZE                                     0x1D00

// SOC Interrupt polarity registers start
#define MT8167_SOC_INT_POL                                  0x620

#define MT8167_USB0_BASE                                    0x11100000
#define MT8167_USB0_LENGTH                                  0x1000

#define MT8167_USBPHY_BASE                                  0x11110000
#define MT8167_USBPHY_LENGTH                                0x1000

#define MT8167_USB0_IRQ                                     104
