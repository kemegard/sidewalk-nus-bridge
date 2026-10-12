/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <nus_bridge.h>
#include <sidewalk.h>
#include <bt_app_callbacks.h>
#include <sid_hal_memory_ifc.h>
#include <string.h>

#include <bluetooth/services/nus.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nus_bridge, CONFIG_SIDEWALK_LOG_LEVEL);

#if DT_HAS_CHOSEN(nordic_nus_uart)
#define NUS_UART_NODE DT_CHOSEN(nordic_nus_uart)
#else
#error "No NUS UART: add chosen 'nordic,nus-uart' to devicetree (e.g. in a board overlay)."
#endif

#define UART_LINE_MAX CONFIG_SID_END_DEVICE_NUS_UART_LINE_MAX
#define UART_TX_BUF_SIZE 1024
#define UART_RX_QUEUE_LEN 4

#define TAG_PHONE_TO_SID "[nus->sidewalk] "
#define TAG_SID_TO_PHONE "[sidewalk->nus] "
#define TAG_UART_TO_PHONE "[uart->nus] "

static const struct device *const uart = DEVICE_DT_GET(NUS_UART_NODE);

struct uart_line {
	uint16_t len;
	uint8_t data[UART_LINE_MAX];
};

K_MSGQ_DEFINE(uart_rx_msgq, sizeof(struct uart_line), UART_RX_QUEUE_LEN, 4);
RING_BUF_DECLARE(uart_tx_rb, UART_TX_BUF_SIZE);

static struct uart_line rx_line;

static struct bt_le_ext_adv *adv;
static struct bt_conn *nus_conn;
static K_MUTEX_DEFINE(nus_conn_mutex);
static atomic_t adv_restart_pending;

static const struct bt_le_adv_param adv_param = {
	.id = NUS_BRIDGE_BT_ID,
	.options = BT_LE_ADV_OPT_CONN,
	.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
	.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_SID_END_DEVICE_NUS_BLE_NAME,
		sizeof(CONFIG_SID_END_DEVICE_NUS_BLE_NAME) - 1),
};

static void uart_rx_work_handler(struct k_work *work);
static void adv_work_handler(struct k_work *work);
static K_WORK_DEFINE(uart_rx_work, uart_rx_work_handler);
static K_WORK_DEFINE(adv_work, adv_work_handler);

/* UART */

/* Writers run in several threads (BT RX, Sidewalk, system workqueue), the ring buffer allows one. */
static struct k_spinlock uart_tx_lock;

static void uart_write_tagged(const char *tag, const uint8_t *data, size_t len)
{
	size_t tag_len = strlen(tag);
	k_spinlock_key_t key = k_spin_lock(&uart_tx_lock);

	if (ring_buf_space_get(&uart_tx_rb) < tag_len + len + 2) {
		k_spin_unlock(&uart_tx_lock, key);
		LOG_WRN("NUS UART tx buffer full, line dropped");
		return;
	}
	ring_buf_put(&uart_tx_rb, (const uint8_t *)tag, tag_len);
	ring_buf_put(&uart_tx_rb, data, len);
	ring_buf_put(&uart_tx_rb, (const uint8_t *)"\r\n", 2);
	uart_irq_tx_enable(uart);
	k_spin_unlock(&uart_tx_lock, key);
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		uint8_t c;

		if (uart_fifo_read(dev, &c, 1) != 1) {
			break;
		}

		bool eol = (c == '\r' || c == '\n');

		if (!eol) {
			rx_line.data[rx_line.len++] = c;
		}

		if ((eol && rx_line.len > 0) || rx_line.len == UART_LINE_MAX) {
			if (k_msgq_put(&uart_rx_msgq, &rx_line, K_NO_WAIT)) {
				LOG_WRN("NUS UART rx queue full, line dropped");
			} else {
				k_work_submit(&uart_rx_work);
			}
			rx_line.len = 0;
		}
	}

	if (uart_irq_tx_ready(dev)) {
		/* Same lock as the writers, so tx is never disabled right after a writer enabled it. */
		k_spinlock_key_t key = k_spin_lock(&uart_tx_lock);
		uint8_t *buf;
		uint32_t len = ring_buf_get_claim(&uart_tx_rb, &buf, UART_TX_BUF_SIZE);

		if (len == 0) {
			uart_irq_tx_disable(dev);
		} else {
			int sent = uart_fifo_fill(dev, buf, len);

			ring_buf_get_finish(&uart_tx_rb, sent > 0 ? sent : 0);
		}
		k_spin_unlock(&uart_tx_lock, key);
	}
}

static int uart_init(void)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("NUS UART device not ready");
		return -ENODEV;
	}

	int err = uart_irq_callback_user_data_set(uart, uart_isr, NULL);

	if (err) {
		LOG_ERR("NUS UART irq callback set failed (err %d)", err);
		return err;
	}
	uart_irq_rx_enable(uart);

	return 0;
}

/* NUS link */

static bool conn_is_nus(struct bt_conn *conn)
{
	struct bt_conn_info cinfo = {};

	if (!conn || bt_conn_get_info(conn, &cinfo)) {
		return false;
	}

	return cinfo.id == NUS_BRIDGE_BT_ID;
}

static struct bt_conn *nus_conn_get(void)
{
	struct bt_conn *conn = NULL;

	k_mutex_lock(&nus_conn_mutex, K_FOREVER);
	if (nus_conn) {
		conn = bt_conn_ref(nus_conn);
	}
	k_mutex_unlock(&nus_conn_mutex);

	return conn;
}

static int nus_send_all(struct bt_conn *conn, const uint8_t *data, size_t len)
{
	uint32_t mtu = bt_nus_get_mtu(conn);

	while (len > 0) {
		uint16_t chunk = MIN(len, mtu);
		int err = bt_nus_send(conn, data, chunk);

		if (err) {
			return err;
		}
		data += chunk;
		len -= chunk;
	}

	return 0;
}

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);

	if (err && err != -EALREADY) {
		LOG_ERR("NUS advertising start failed (err %d)", err);
		return;
	}

	LOG_INF("NUS advertising as \"%s\"", CONFIG_SID_END_DEVICE_NUS_BLE_NAME);
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	if (!conn_is_nus(conn)) {
		return;
	}

	if (conn_err) {
		LOG_ERR("NUS connection failed (err %u)", conn_err);
		atomic_set(&adv_restart_pending, 1);
		return;
	}

	k_mutex_lock(&nus_conn_mutex, K_FOREVER);
	nus_conn = bt_conn_ref(conn);
	k_mutex_unlock(&nus_conn_mutex);

	LOG_INF("NUS connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (!conn_is_nus(conn)) {
		return;
	}

	k_mutex_lock(&nus_conn_mutex, K_FOREVER);
	if (nus_conn == conn) {
		bt_conn_unref(nus_conn);
		nus_conn = NULL;
	}
	k_mutex_unlock(&nus_conn_mutex);

	LOG_INF("NUS disconnected (reason 0x%02x)", reason);
	atomic_set(&adv_restart_pending, 1);
}

static void recycled(void)
{
	/* Restart only once the connection object is free again, so BT_MAX_CONN is not exceeded. */
	if (atomic_cas(&adv_restart_pending, 1, 0)) {
		k_work_submit(&adv_work);
	}
}

BT_CONN_CB_DEFINE(nus_bridge_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

/* Data paths */

static void free_uplink_ctx(void *ctx)
{
	sidewalk_msg_t *uplink = (sidewalk_msg_t *)ctx;

	if (uplink == NULL) {
		return;
	}
	if (uplink->msg.data) {
		sid_hal_free(uplink->msg.data);
	}
	sid_hal_free(uplink);
}

static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	if (!conn_is_nus(conn)) {
		return;
	}

	LOG_INF("NUS rx %u bytes, send to Sidewalk", len);
	uart_write_tagged(TAG_PHONE_TO_SID, data, len);

	sidewalk_msg_t *uplink = sid_hal_malloc(sizeof(sidewalk_msg_t));

	if (!uplink) {
		LOG_ERR("Failed to alloc memory for message context");
		return;
	}
	memset(uplink, 0x0, sizeof(*uplink));

	uplink->msg.size = len;
	uplink->msg.data = sid_hal_malloc(uplink->msg.size);
	if (!uplink->msg.data) {
		sid_hal_free(uplink);
		LOG_ERR("Failed to allocate memory for message data");
		return;
	}
	memcpy(uplink->msg.data, data, uplink->msg.size);

	uplink->desc.type = SID_MSG_TYPE_NOTIFY;
	uplink->desc.link_type = SID_LINK_TYPE_ANY;
	uplink->desc.link_mode = SID_LINK_MODE_CLOUD;

	int err = sidewalk_event_send(sidewalk_event_send_msg, uplink, free_uplink_ctx);

	if (err) {
		free_uplink_ctx(uplink);
		LOG_ERR("Send event err %d", err);
	}
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
};

static void uart_rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct uart_line line;

	while (k_msgq_get(&uart_rx_msgq, &line, K_NO_WAIT) == 0) {
		struct bt_conn *conn = nus_conn_get();

		if (!conn) {
			LOG_WRN("UART line dropped, no NUS peer connected");
			continue;
		}

		int err = nus_send_all(conn, line.data, line.len);

		bt_conn_unref(conn);
		if (err) {
			LOG_WRN("NUS send failed (err %d)", err);
			continue;
		}
		uart_write_tagged(TAG_UART_TO_PHONE, line.data, line.len);
	}
}

void nus_bridge_sid_downlink(const uint8_t *data, size_t len)
{
	uart_write_tagged(TAG_SID_TO_PHONE, data, len);

	struct bt_conn *conn = nus_conn_get();

	if (!conn) {
		LOG_WRN("Sidewalk downlink not forwarded, no NUS peer connected");
		return;
	}

	int err = nus_send_all(conn, data, len);

	bt_conn_unref(conn);
	if (err) {
		LOG_WRN("NUS send failed (err %d)", err);
	}
}

bool nus_bridge_attr_is_NUS(const struct bt_gatt_attr *attr)
{
	return bt_uuid_cmp(attr->uuid, BT_UUID_NUS_TX) == 0 ||
	       bt_uuid_cmp(attr->uuid, BT_UUID_NUS_RX) == 0;
}

int nus_bridge_start(void)
{
	int err = sid_ble_bt_enable(NULL);

	if (err && err != -EALREADY) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
		return err;
	}

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("NUS init failed (err %d)", err);
		return err;
	}

	err = uart_init();
	if (err) {
		return err;
	}

	err = bt_le_ext_adv_create(&adv_param, NULL, &adv);
	if (err) {
		LOG_ERR("NUS advertising create failed (err %d)", err);
		return err;
	}

	err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("NUS advertising data set failed (err %d)", err);
		return err;
	}

	k_work_submit(&adv_work);

	return 0;
}
