/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <nus_bridge.h>
#include <sidewalk.h>
#include <bt_app_callbacks.h>
#include <sid_hal_memory_ifc.h>
#include <app_mfg_config.h>
#include <json_printer/sidTypes2str.h>
#include <stdarg.h>
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

#define TAG_NUS_RX "[nus] "
#define TAG_SID_TO_PHONE "[sidewalk->nus] "
#define TAG_UART_TO_PHONE "[uart->nus] "
#define TAG_CMD "[cmd] "

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

/* Local commands, all handlers run on the Sidewalk thread */

#define CMD_REPLY_MAX 160
#define PING_PAYLOAD "ping"
#define PING_RETRIES 3
#define PING_TTL_S 60

/* Id of the !ping message waiting for its ack, 0 if none. Sidewalk thread only. */
static uint16_t ping_msg_id;

static void cmd_reply(const char *fmt, ...)
{
	char buf[CMD_REPLY_MAX];
	va_list args;

	va_start(args, fmt);
	int len = vsnprintk(buf, sizeof(buf), fmt, args);
	va_end(args);
	len = MIN(len, (int)sizeof(buf) - 1);

	uart_write_tagged(TAG_CMD, (const uint8_t *)buf, len);

	struct bt_conn *conn = nus_conn_get();

	if (!conn) {
		return;
	}
	int err = nus_send_all(conn, (const uint8_t *)buf, len);

	bt_conn_unref(conn);
	if (err) {
		LOG_WRN("NUS reply failed (err %d)", err);
	}
}

static const char *link_up_str(const struct sid_status *st, uint32_t link)
{
	return (st->detail.link_status_mask & link) ? "up" : "down";
}

static const char *sid_state_str(enum sid_state state)
{
	switch (state) {
	case SID_STATE_READY:
		return "ready";
	case SID_STATE_NOT_READY:
		return "not ready";
	case SID_STATE_ERROR:
		return "error";
	case SID_STATE_SECURE_CHANNEL_READY:
		return "secure channel ready";
	default:
		return "unknown";
	}
}

/* Explains why Sidewalk has no handle; false if it is running. */
static bool cmd_not_started(sidewalk_ctx_t *sid, const char *cmd)
{
	if (sid->handle != NULL) {
		return false;
	}
	cmd_reply("%s: sidewalk not started (%s)", cmd,
		  app_mfg_cfg_is_empty() ? "not provisioned, see log for the guide" :
					   "init failed, see log");
	return true;
}

static void cmd_status(sidewalk_ctx_t *sid)
{
	if (cmd_not_started(sid, "status")) {
		return;
	}

	struct sid_status st = sid->last_status;
	sid_error_t e = sid_get_status(sid->handle, &st);

	if (e) {
		LOG_WRN("sid_get_status err %d, using last status", (int)e);
	}

	cmd_reply("status: %s, %sregistered, time %s, BLE %s, FSK %s, LoRa %s",
		  sid_state_str(st.state),
		  st.detail.registration_status == SID_STATUS_REGISTERED ? "" : "not ",
		  st.detail.time_sync_status == SID_STATUS_TIME_SYNCED ? "synced" : "not synced",
		  link_up_str(&st, SID_LINK_TYPE_1), link_up_str(&st, SID_LINK_TYPE_2),
		  link_up_str(&st, SID_LINK_TYPE_3));
}

static void cmd_mtu(sidewalk_ctx_t *sid)
{
	static const struct {
		uint32_t link;
		const char *name;
	} links[] = {
		{ SID_LINK_TYPE_1, "BLE" },
		{ SID_LINK_TYPE_2, "FSK" },
		{ SID_LINK_TYPE_3, "LoRa" },
	};

	if (cmd_not_started(sid, "mtu")) {
		return;
	}

	char buf[CMD_REPLY_MAX];
	int pos = 0;

	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		if (!(sid->config.link_mask & links[i].link)) {
			continue;
		}
		size_t mtu = 0;
		sid_error_t e = sid_get_mtu(sid->handle, links[i].link, &mtu);

		if (e) {
			pos += snprintk(buf + pos, sizeof(buf) - pos, " %s n/a (%s)", links[i].name,
					SID_ERROR_T_STR(e));
		} else {
			pos += snprintk(buf + pos, sizeof(buf) - pos, " %s %u bytes", links[i].name,
					(unsigned)mtu);
		}
		pos = MIN(pos, (int)sizeof(buf) - 1);
	}

	cmd_reply("mtu:%s", pos ? buf : " no link enabled");
}

static void cmd_ping(sidewalk_ctx_t *sid)
{
	if (cmd_not_started(sid, "ping")) {
		return;
	}
	if (ping_msg_id) {
		cmd_reply("ping: still waiting for ack of message %u", ping_msg_id);
		return;
	}

	struct sid_msg msg = {
		.data = (void *)PING_PAYLOAD,
		.size = sizeof(PING_PAYLOAD) - 1,
	};
	struct sid_msg_desc desc = {
		.type = SID_MSG_TYPE_NOTIFY,
		.link_type = SID_LINK_TYPE_ANY,
		.link_mode = SID_LINK_MODE_CLOUD,
		.msg_desc_attr.tx_attr = {
			.request_ack = true,
			.num_retries = PING_RETRIES,
			.ttl_in_seconds = PING_TTL_S,
		},
	};
	sid_error_t e = sid_put_msg(sid->handle, &msg, &desc);

	if (e) {
		cmd_reply("ping: send failed %d (%s)", (int)e, SID_ERROR_T_STR(e));
		return;
	}

	ping_msg_id = desc.id;
	cmd_reply("ping: message %u queued, waiting for ack (up to %u s)", desc.id, PING_TTL_S);
}

#define CMD_LIST "!status !mtu !ping !help"

static void cmd_event(sidewalk_ctx_t *sid, void *ctx)
{
	const char *cmd = (const char *)ctx;

	if (strcmp(cmd, "!status") == 0) {
		cmd_status(sid);
	} else if (strcmp(cmd, "!mtu") == 0) {
		cmd_mtu(sid);
	} else if (strcmp(cmd, "!ping") == 0) {
		cmd_ping(sid);
	} else if (strcmp(cmd, "!help") == 0) {
		cmd_reply("commands: " CMD_LIST);
	} else {
		/* Everything else is ignored, nothing is sent to Sidewalk. */
		cmd_reply("ignored, allowed commands: " CMD_LIST);
	}
}

/* Data paths */

static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	if (!conn_is_nus(conn)) {
		return;
	}

	uart_write_tagged(TAG_NUS_RX, data, len);

	/* Drop trailing line endings and spaces added by terminal apps. */
	while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n' || data[len - 1] == ' ')) {
		len--;
	}

	char *cmd = sid_hal_malloc(len + 1);

	if (!cmd) {
		LOG_ERR("Failed to alloc memory for command");
		return;
	}
	memcpy(cmd, data, len);
	cmd[len] = '\0';

	LOG_INF("NUS rx %u bytes", len);

	int err = sidewalk_event_send(cmd_event, cmd, sid_hal_free);

	if (err) {
		sid_hal_free(cmd);
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

static void sid_downlink(const uint8_t *data, size_t len)
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

void nus_bridge_sid_msg_received(const struct sid_msg_desc *msg_desc, const struct sid_msg *msg)
{
	/* Acks carry no payload, they only complete a pending !ping. */
	if (msg_desc->msg_desc_attr.rx_attr.is_msg_ack) {
		if (ping_msg_id && msg_desc->id == ping_msg_id) {
			ping_msg_id = 0;
			cmd_reply("ping: ack received for message %u", msg_desc->id);
		}
		return;
	}

	sid_downlink((const uint8_t *)msg->data, msg->size);
}

void nus_bridge_sid_msg_sent(const struct sid_msg_desc *msg_desc)
{
	if (ping_msg_id && msg_desc->id == ping_msg_id) {
		cmd_reply("ping: message %u sent, waiting for ack", msg_desc->id);
	}
}

void nus_bridge_sid_send_error(sid_error_t error, const struct sid_msg_desc *msg_desc)
{
	if (ping_msg_id && msg_desc->id == ping_msg_id) {
		ping_msg_id = 0;
		cmd_reply("ping: message %u failed %d (%s)", msg_desc->id, (int)error,
			  SID_ERROR_T_STR(error));
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
