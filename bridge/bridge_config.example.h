/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copy this file to bridge_config.h before making a deployment build.
 * Use the address printed by the generic bridge's 115200-baud serial log.
 * Keep the bytes in the same left-to-right order as the printed address.
 */

#pragma once

#define HSC_SENSOR_ADDRESS_FILTER_ENABLED true
#define HSC_SENSOR_ADDRESS_BYTES 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
