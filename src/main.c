/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Nordic UART Service Client sample
 */
#include <uart_async_adapter.h>

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_client.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>

#include <zephyr/settings/settings.h>

#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log.h>

#include <cmsis_core.h>
#include <zephyr/arch/arm/exception.h>

#define LOG_MODULE_NAME central_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_DBG);

#define STACKSIZE 4096
#define PRIORITY 7

/* UART payload buffer element size. */
// #define UART_BUF_SIZE 384
#define UART_BUF_SIZE 740
#define BT_NUS_UART_BUFFER_SIZE 40

#define KEY_PASSKEY_ACCEPT DK_BTN1_MSK
#define KEY_PASSKEY_REJECT DK_BTN2_MSK

#define NUS_WRITE_TIMEOUT K_MSEC(150)
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_RX_TIMEOUT 50000 /* Wait for RX complete event time in microseconds. */

static const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(nordic_nus_uart));
static struct k_work_delayable uart_work;

K_SEM_DEFINE(nus_write_sem, 0, 1);
static K_SEM_DEFINE(ble_init_ok, 0, 1);

#ifdef CONFIG_UART_ASYNC_ADAPTER
UART_ASYNC_ADAPTER_INST_DEFINE(async_adapter);
#else
#define async_adapter NULL
#endif

struct uart_data_t {
	void *fifo_reserved;
	uint8_t  data[UART_BUF_SIZE];
	uint16_t len;
};
struct nus_data_t{
	void *fifo_reserved;
	uint8_t data[BT_NUS_UART_BUFFER_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

static struct bt_conn *default_conn;
static struct bt_nus_client nus_client;

static void ble_data_sent(struct bt_nus_client *nus, uint8_t err,
					const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(nus);
	ARG_UNUSED(data);
	// ARG_UNUSED(len);
	LOG_DBG("BLE data sent, len: %d", len);
	k_sem_give(&nus_write_sem);

	if (err) {
		LOG_WRN("ATT error code: 0x%02X", err);
	}
}

static uint8_t ble_data_received(struct bt_nus_client *nus,
						const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(nus);
	// int err;
	// LOG_DBG("BLE data rcvd, len: %d", len);
	struct nus_data_t *buf = k_malloc(sizeof(*buf));
	if (!buf) {
		LOG_WRN("Not able to allocate UART send data buffer");
		return BT_GATT_ITER_CONTINUE;
	}
	memcpy(&buf->data, data, len);
	// LOG_DBG("UART TX Len: %d", tx->len);		
	// LOG_DBG("UART TX Pos: %d", pos);
	// err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
	// if (err) {
	// 	LOG_DBG("UART TX err: %d", err);		
	// 	k_fifo_put(&fifo_uart_tx_data, tx);
	// }
	buf->len = len;

	LOG_DBG("UART TX -> FIFO, len: %u", buf->len);
	k_fifo_put(&fifo_uart_tx_data, buf);

	return BT_GATT_ITER_CONTINUE;
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);

	static size_t aborted_len;
	struct uart_data_t *buf;
	static uint8_t *aborted_buf;
	static bool disable_req;

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("UART_TX_DONE");
		if ((evt->data.tx.len == 0) ||
		    (!evt->data.tx.buf)) {
			return;
		}

		if (aborted_buf) {
			buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
					   data[0]);
			aborted_buf = NULL;
			aborted_len = 0;
		} else {
			buf = CONTAINER_OF(evt->data.tx.buf,
					   struct uart_data_t,
					   data[0]);
		}

		k_free(buf);

		// buf = k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
		// // buf = k_fifo_get(&fifo_uart_tx_data, K_MSEC(5));
		// if (!buf) {
		// 	return;
		// }

		// if (uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS)) {
		// 	LOG_WRN("Failed to send data over UART");
		// }

		break;

	case UART_RX_RDY:
		LOG_DBG("UART_RX_RDY");
		buf = CONTAINER_OF(evt->data.rx.buf, struct uart_data_t, data[0]);
		buf->len += evt->data.rx.len;
		LOG_DBG("UART_RX_RDY, len: %d", evt->data.rx.len);

		if (disable_req) {
			return;
		}

		// if ((evt->data.rx.buf[buf->len - 1] == '\n') ||
		//     (evt->data.rx.buf[buf->len - 1] == '\r')) {
		// 	disable_req = true;
		// 	uart_rx_disable(uart);
		// }
		disable_req = true;
		uart_rx_disable(uart);

		break;

	case UART_RX_DISABLED:
		LOG_DBG("UART_RX_DISABLED");
		disable_req = false;

		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
			k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
			return;
		}

		uart_rx_enable(uart, buf->data, sizeof(buf->data),
			       UART_RX_TIMEOUT);

		break;

	case UART_RX_BUF_REQUEST:
		LOG_DBG("UART_RX_BUF_REQUEST");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
			uart_rx_buf_rsp(uart, buf->data, sizeof(buf->data));
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
		}

		break;

	case UART_RX_BUF_RELEASED:
		buf = CONTAINER_OF(evt->data.rx_buf.buf, struct uart_data_t,
				   data[0]);
		LOG_DBG("UART_RX_BUF_RELEASED, len: %d", buf->len);

		if (buf->len > 0) {
			k_fifo_put(&fifo_uart_rx_data, buf);
		} else {
			k_free(buf);
		}

		break;

	case UART_TX_ABORTED:
		LOG_DBG("UART_TX_ABORTED");
		if (!aborted_buf) {
			aborted_buf = (uint8_t *)evt->data.tx.buf;
		}

		aborted_len += evt->data.tx.len;
		buf = CONTAINER_OF((void *)aborted_buf, struct uart_data_t,
				   data);
        // LOG_DBG("retry UART TX Len: %d", buf->len - aborted_len);	
		uart_tx(uart, &buf->data[aborted_len],
			buf->len - aborted_len, SYS_FOREVER_MS);

		break;

	default:
		break;
	}
}

static void uart_work_handler(struct k_work *item)
{
	struct uart_data_t *buf;

	buf = k_malloc(sizeof(*buf));
	if (buf) {
		buf->len = 0;
	} else {
		LOG_WRN("Not able to allocate UART receive buffer(work handler)");
		k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
		return;
	}

	uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_RX_TIMEOUT);
}

static bool uart_test_async_api(const struct device *dev)
{
	const struct uart_driver_api *api =
			(const struct uart_driver_api *)dev->api;

	return (api->callback_set != NULL);
}

static int uart_init(void)
{
	int err;
	struct uart_data_t *rx;

	if (!device_is_ready(uart)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	rx = k_malloc(sizeof(*rx));
	if (rx) {
		rx->len = 0;
	} else {
		return -ENOMEM;
	}

	k_work_init_delayable(&uart_work, uart_work_handler);
	
	if (IS_ENABLED(CONFIG_UART_ASYNC_ADAPTER) && !uart_test_async_api(uart)) {
		/* Implement API adapter */
		uart_async_adapter_init(async_adapter, uart);
		uart = async_adapter;
	}

	err = uart_callback_set(uart, uart_cb, NULL);
	if (err) {
		return err;
	}
	if (IS_ENABLED(CONFIG_UART_LINE_CTRL)) {
		LOG_INF("Wait for DTR");
		while (true) {
			uint32_t dtr = 0;

			uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr);
			if (dtr) {
				break;
			}
			/* Give CPU resources to low priority threads. */
			k_sleep(K_MSEC(100));
		}
		
		LOG_INF("DTR set");
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DCD, 1);
		if (err) {
			LOG_WRN("Failed to set DCD, ret code %d", err);
		}
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DSR, 1);
		if (err) {
			LOG_WRN("Failed to set DSR, ret code %d", err);
		}
	}
	
	err = uart_rx_enable(uart, rx->data, sizeof(rx->data), UART_RX_TIMEOUT);
	if (err) {
		LOG_ERR("Cannot enable uart reception (err: %d)", err);
		/* Free the rx buffer only because the tx buffer will be handled in the callback */
		k_free(rx);
	}

	return err;
}

static void discovery_complete(struct bt_gatt_dm *dm,
			       void *context)
{
	struct bt_nus_client *nus = context;
	LOG_INF("Service discovery completed");

	bt_gatt_dm_data_print(dm);

	bt_nus_handles_assign(dm, nus);
	bt_nus_subscribe_receive(nus);

	bt_gatt_dm_data_release(dm);
}

static void discovery_service_not_found(struct bt_conn *conn,
					void *context)
{
	LOG_INF("Service not found");
}

static void discovery_error(struct bt_conn *conn,
			    int err,
			    void *context)
{
	LOG_WRN("Error while discovering GATT database: (%d)", err);
}

struct bt_gatt_dm_cb discovery_cb = {
	.completed         = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found       = discovery_error,
};

static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != default_conn) {
		return;
	}

	err = bt_gatt_dm_start(conn,
			       BT_UUID_NUS_SERVICE,
			       &discovery_cb,
			       &nus_client);
	if (err) {
		LOG_ERR("could not start the discovery procedure, error "
			"code: %d", err);
	}
}

static void exchange_func(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	if (!err) {
		LOG_INF("MTU exchange done");
		LOG_INF("MTU exchange done");

	} else {
		LOG_WRN("MTU exchange failed (err %" PRIu8 ")", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		LOG_INF("Failed to connect to %s (%d)", addr, conn_err);

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
			if (err) {
				LOG_ERR("Scanning failed to start (err %d)",
					err);
			}
		}

		return;
	}

	LOG_INF("Connected: %s", addr);

	static struct bt_gatt_exchange_params exchange_params;

	exchange_params.func = exchange_func;
	err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (err) {
		LOG_WRN("MTU exchange failed (err %d)", err);
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Failed to set security: %d", err);

		gatt_discover(conn);
	}

	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY)) {
		LOG_ERR("Stop LE scan failed (err %d)", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", addr, reason);

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)",
			err);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d", addr,
			level, err);
	}

	gatt_discover(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed
};

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	LOG_INF("Filters matched. Address: %s connectable: %d",
		addr, connectable);
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	LOG_WRN("Connecting failed");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	default_conn = bt_conn_ref(conn);
}

static int nus_client_init(void)
{
	int err;
	struct bt_nus_client_init_param init = {
		.cb = {
			.received = ble_data_received,
			.sent = ble_data_sent,
		}
	};

	err = bt_nus_client_init(&nus_client, &init);
	if (err) {
		LOG_ERR("NUS Client initialization failed (err %d)", err);
		return err;
	}

	LOG_INF("NUS Client module initialized");
	return err;
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL,
		scan_connecting_error, scan_connecting);

static int scan_init(void)
{
	int err;
	struct bt_scan_init_param scan_init = {
		.connect_if_match = 1,
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_NUS_SERVICE);
	if (err) {
		LOG_ERR("Scanning filters cannot be set (err %d)", err);
		return err;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		LOG_ERR("Filters cannot be turned on (err %d)", err);
		return err;
	}

	LOG_INF("Scan module initialized");
	return err;
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_WRN("Pairing failed conn: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};

int debug_mon_enable(void)
{
	/*
	 * Cannot enable monitor mode if C_DEBUGEN bit is set. This bit can only be
	 * altered from debug access port. It is cleared on power-on-reset.
	 */
	bool is_in_halting_mode = (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) == 1;

	if (is_in_halting_mode) {
		return -1;
	}

	/* Enable monitor mode debugging by setting MON_EN bit of DEMCR */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_MON_EN_Msk;
	return 0;
}

int main(void)
{
	int err;
	/* Set up debug monitor */
	err = debug_mon_enable();
	if (err) {
		LOG_ERR("Error enabling monitor mode:\n		Cannot enable DBM when CPU is in Debug mode");
		return 0;
	}

	err = uart_init();
	if (err != 0) {
		LOG_ERR("uart_init failed (err %d)", err);
		return 0;
	}

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("Failed to register authorization callbacks.");
		return 0;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	LOG_INF("Bluetooth initialized");

	k_sem_give(&ble_init_ok);
	err = scan_init();
	if (err != 0) {
		LOG_ERR("scan_init failed (err %d)", err);
		return 0;
	}

	err = nus_client_init();
	if (err != 0) {
		LOG_ERR("nus_client_init failed (err %d)", err);
		return 0;
	}

	printk("Starting Bluetooth Central UART example\n");

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return 0;
	}

	LOG_INF("Scanning successfully started");

	struct nus_data_t nus_data = {
		.len = 0,
	};
	for (;;) {
		/* Wait indefinitely for data to be sent over Bluetooth */
		struct uart_data_t *buf = k_fifo_get(&fifo_uart_rx_data,
						     K_FOREVER);

		int plen = MIN(sizeof(nus_data.data) - nus_data.len, buf->len);
		int loc = 0;

		while (plen > 0) {
			memcpy(&nus_data.data[nus_data.len], &buf->data[loc], plen);
			nus_data.len += plen;
			loc += plen;
			// if (nus_data.len >= sizeof(nus_data.data)) {
			err = bt_nus_client_send(&nus_client, nus_data.data, nus_data.len);
			if (err) {
				LOG_WRN("Failed to send data over BLE connection"
					"(err %d)", err);
			}

			err = k_sem_take(&nus_write_sem, NUS_WRITE_TIMEOUT);
			if (err) {
				LOG_WRN("NUS send timeout");
			}

			nus_data.len = 0;
			// }

			plen = MIN(sizeof(nus_data.data), buf->len - loc);
		}

		k_free(buf);
	}
}
void ble_read_thread(void)
{
	// struct uart_data_t uart_data = {
	// 	.len = 0,
	// };
	int32_t err = 0;
	volatile uint8_t i = 0;
	volatile uint8_t j = 0;
	volatile uint16_t uart_loc = 0;
	volatile uint16_t nus_loc = 0;
	volatile bool buffer_full = false;
	volatile size_t plen = 0;

	// uint8_t *uart_buf;
	// uint8_t *uart_buf = NULL;
	// size_t uart_buf_size = (size_t)(sizeof(uint8_t) * UART_BUF_SIZE);
	size_t uart_buf_size = sizeof(struct uart_data_t);

	// err = sizeof(buf);
	struct nus_data_t *buffer[10] = {NULL};
	// *buf = k_malloc(sizeof(*buf));

	// struct nus_data_t *test = k_malloc(sizeof(*test));
	// struct nus_data_t *test2 = NULL;
	for (;;) {
		/* Wait indefinitely for data to be sent over Bluetooth */
		i = 0;
		j = 0;
		buffer[i] = k_fifo_get(&fifo_uart_tx_data, K_FOREVER);
		// LOG_DBG("buffer[%d], addr: 0x%X size: %hu len: %hu", i, (uint32_t)&buffer[i], sizeof(*buffer[i]), buffer[i]->len);

		struct uart_data_t *uart_buf = k_malloc(uart_buf_size);
		if(!uart_buf){
			LOG_WRN("Could not allocate UART tx buffer!");
			k_free(buffer[i]);
			break;
		}
		memset(&uart_buf->data, 0, sizeof(uart_buf->data));
		uart_buf->len = 0;
		
		i++;
		//wait for all data
		// LOG_DBG("wait for all data");
		k_msleep(50);
		// int is_empty = k_fifo_is_empty(&fifo_uart_tx_data);
		// (void)is_empty;
		while(!k_fifo_is_empty(&fifo_uart_tx_data)){
			buffer[i] = k_fifo_get(&fifo_uart_tx_data, K_MSEC(50));
			// test = k_fifo_get(&fifo_uart_tx_data, K_MSEC(50));
			if(buffer[i]){
				// LOG_DBG("Fetched uart tx buffer from fifo, len: %u", buffer[i]->len);
				// LOG_DBG("buffer[%d], addr: 0x%X size: %hu len: %hu", i, (uint32_t)&buffer[i], sizeof(*buffer[i]), buffer[i]->len);
				// test = NULL;
				i++;
			}
			else{
				LOG_DBG("could not fetch buffer from fifo!");
			}
		}
		buffer[i] = 0;
		// LOG_DBG("Fetched uart tx buffers from fifo");		
		// Pack data into UART TX buffer and send
		plen = 0;
		uart_loc = 0;
		nus_loc = 0;

		do{
			while(!buffer_full){
				// LOG_DBG("uart_buf_size: %hu, uart_buf->len: %hu, buffer[j]->len: %hu, nus_loc: %hu", 
				// (uint16_t)uart_buf_size, (uint16_t)uart_buf->len, (uint16_t)buffer[j]->len, (uint16_t)nus_loc);
				// LOG_DBG("uart_buf_size - uart_buf->len = %hu, buffer[j]->len - nus_loc = %hu", sizeof(uart_buf->data) - uart_buf->len, buffer[j]->len - nus_loc);
				plen = MIN(sizeof(uart_buf->data) - uart_buf->len, buffer[j]->len - nus_loc);
				// LOG_DBG("Filling uart_buf, uart_buf->len:%hu plen: %hu", uart_buf->len, plen);
				memcpy(&uart_buf->data[uart_buf->len], &(buffer[j]->data[nus_loc]), plen);
				
				if(sizeof(uart_buf->data) <= (uart_buf->len + plen)){
					// uart buffer is full				
					buffer_full = true;
					LOG_DBG("uart tx buffer full");
				}
				else if(buffer[j]->len <= (nus_loc + plen)){
					// End of current NUS packet payload
					nus_loc = 0;
					uart_buf->len += plen;
					k_free(buffer[j]);
					if(!buffer[++j]){
						break;
					}
					// j++;
					LOG_DBG("end of NUS packet");
					// break;
					// continue;
				}
				else{
					uart_buf->len += plen;
					nus_loc += plen;					
				}
			}
			if(buffer_full){
				// send uart data, block if buisy
				do{
					// LOG_DBG("UART TX, len: %u", uart_loc);
					err = uart_tx(uart, uart_buf->data, uart_buf->len, SYS_FOREVER_MS);
					if ((err) && (err != -EBUSY)) {
						LOG_DBG("UART TX err: %d", err);
						break;
					}
					k_msleep(5);
				}while(err == -EBUSY);				
				// allocate new buffer, prev will be freed in cb.
				do{
					struct uart_data_t *uart_buf = k_malloc(uart_buf_size);
					if(!uart_buf){
						LOG_WRN("Could not allocate UART tx buffer!");
						k_free(buffer[j]);
						break;
					}
					memset(&uart_buf->data, 0, sizeof(uart_buf->data));
					uart_buf->len = 0;

				}while(!uart_buf);
				
				uart_buf->len = 0;
				buffer_full = false;
			}
		}while(buffer[j]);

		if(uart_buf->len){
			// send partially filled UART TX buffer
			// LOG_DBG("UART TX(partial), len: %u", uart_buf->len);
			do{
				err = uart_tx(uart, uart_buf->data, uart_buf->len, SYS_FOREVER_MS);
				if (err) {
					LOG_DBG("UART TX err: %d", err);		
				}
				k_msleep(5);
			}while(err == -EBUSY);
		}		
	}
}
K_THREAD_DEFINE(ble_read_thread_id, STACKSIZE, ble_read_thread, NULL, NULL,
		NULL, PRIORITY, 0, 0);