/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#ifndef NUS_BRIDGE_H
#define NUS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

/**
 * @brief BT identity used by the NUS link.
 * Sidewalk uses BT_ID_SIDEWALK (and BT_ID_SMP_DFU with DFU), the default identity is free.
 */
#define NUS_BRIDGE_BT_ID BT_ID_DEFAULT

/**
 * @brief Enable BT, start NUS advertising and the NUS UART.
 * Must be called from the Sidewalk event thread, because sid_ble_bt_enable is not thread safe.
 *
 * @return 0 on success, negative errno otherwise
 */
int nus_bridge_start(void);

/**
 * @brief Forward a Sidewalk downlink to the NUS peer and the NUS UART.
 *
 * @param data message payload
 * @param len payload length
 */
void nus_bridge_sid_downlink(const uint8_t *data, size_t len);

/**
 * @brief check if attr is a NUS characteristic value
 *
 * @param attr
 * @return true if attr belongs to NUS
 * @return false in other cases
 */
bool nus_bridge_attr_is_NUS(const struct bt_gatt_attr *attr);

#endif /* NUS_BRIDGE_H */
