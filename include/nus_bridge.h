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
#include <sid_api.h>

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
 * @brief Sidewalk on_msg_received hook.
 * Completes a pending !ping on its ack, forwards other downlinks to the NUS peer and the NUS UART.
 * Call from the Sidewalk thread.
 *
 * @param msg_desc message descriptor
 * @param msg message
 */
void nus_bridge_sid_msg_received(const struct sid_msg_desc *msg_desc, const struct sid_msg *msg);

/**
 * @brief Sidewalk on_msg_sent hook, reports !ping progress. Call from the Sidewalk thread.
 *
 * @param msg_desc message descriptor
 */
void nus_bridge_sid_msg_sent(const struct sid_msg_desc *msg_desc);

/**
 * @brief Sidewalk on_send_error hook, reports a failed !ping. Call from the Sidewalk thread.
 *
 * @param error send error
 * @param msg_desc message descriptor
 */
void nus_bridge_sid_send_error(sid_error_t error, const struct sid_msg_desc *msg_desc);

/**
 * @brief check if attr is a NUS characteristic value
 *
 * @param attr
 * @return true if attr belongs to NUS
 * @return false in other cases
 */
bool nus_bridge_attr_is_NUS(const struct bt_gatt_attr *attr);

#endif /* NUS_BRIDGE_H */
