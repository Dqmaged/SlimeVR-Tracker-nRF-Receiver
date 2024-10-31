#include "globals.h"
#include "system.h"
#include "connection.h"

#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/crc.h>

#include "esb.h"

uint8_t last_reset = 0;
const nrfx_timer_t m_timer = NRFX_TIMER_INSTANCE(1);
bool esb_state = false;
bool timer_state = false;
bool send_data = false;
uint16_t led_clock = 0;
uint32_t led_clock_offset = 0;

uint32_t tx_errors = 0;

static struct esb_payload rx_payload;
static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0,
	0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_pair = ESB_CREATE_PAYLOAD(0,
	0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_timer = ESB_CREATE_PAYLOAD(0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
static struct esb_payload tx_payload_sync = ESB_CREATE_PAYLOAD(0,
	0, 0, 0, 0);

static uint8_t paired_addr[8] = {0};

LOG_MODULE_REGISTER(esb_event, LOG_LEVEL_INF);

void event_handler(struct esb_evt const *event)
{
	switch (event->evt_id) {
	case ESB_EVENT_TX_SUCCESS:
		LOG_DBG("TX SUCCESS");
		break;
	case ESB_EVENT_TX_FAILED:
		LOG_DBG("TX FAILED");
		break;
	case ESB_EVENT_RX_RECEIVED:
	// make tx payload for ack here
		if (!esb_read_rx_payload(&rx_payload)) {
			switch (rx_payload.length)
			{
			case 8:
				LOG_INF("RX Pairing Packet");
				memcpy(pairing_buf, rx_payload.data, 8);
				esb_write_payload(&tx_payload_pair); // Add to TX buffer
				break;
			case 16:
				uint8_t imu_id = rx_payload.data[1];
				if (discovered_trackers[imu_id] < DETECTION_THRESHOLD) { // garbage filtering of nonexistent tracker
					discovered_trackers[imu_id]++;
					return;
				}
				if (rx_payload.data[0] > 223) // reserved for receiver only
					break;
				memcpy(&report.data, &rx_payload.data, 16); // all data can be passed through
				if (rx_payload.data[0] != 1) // packet 1 is full precision quat and accel, no room for rssi
					report.data[15]=rx_payload.rssi;
				// TODO: this sucks
				for (int i = 0; i < report_count; i++) { // replace existing entry instead
					if (reports[sizeof(report) * (report_sent + i) + 1] == report.data[1]) {
						memcpy(&reports[sizeof(report) * (report_sent + i)], &report, sizeof(report));
//						k_work_submit(&report_send);
						break;
					}
				}
				if (report_count > 100) // overflow
					break;
				memcpy(&reports[sizeof(report) * (report_sent + report_count)], &report, sizeof(report));
				report_count++;
//				k_work_submit(&report_send);
				break;
			default:
				break;
			}
		} else {
			LOG_ERR("Error while reading rx packet");
		}
		break;
	}
}

int clocks_start(void)
{
	int err;
	int res;
	struct onoff_manager *clk_mgr;
	struct onoff_client clk_cli;
	int fetch_attempts = 0;

	clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (!clk_mgr)
	{
		LOG_ERR("Unable to get the Clock manager");
		return -ENXIO;
	}

	sys_notify_init_spinwait(&clk_cli.notify);

	err = onoff_request(clk_mgr, &clk_cli);
	if (err < 0)
	{
		LOG_ERR("Clock request failed: %d", err);
		return err;
	}

	do
	{
		err = sys_notify_fetch_result(&clk_cli.notify, &res);
		if (!err && res)
		{
			LOG_ERR("Clock could not be started: %d", res);
			return res;
		}
		if (err && ++fetch_attempts > 10000) {
			LOG_WRN("Unable to fetch Clock request result: %d", err);
			return err;
		}
	} while (err);

	LOG_DBG("HF clock started");
	return 0;
}

// this was randomly generated
// TODO: I have no idea?
static const uint8_t discovery_base_addr_0[4] = {0x62, 0x39, 0x8A, 0xF2};
static const uint8_t discovery_base_addr_1[4] = {0x28, 0xFF, 0x50, 0xB8};
static const uint8_t discovery_addr_prefix[8] = {0xFE, 0xFF, 0x29, 0x27, 0x09, 0x02, 0xB2, 0xD6};

static uint8_t base_addr_0[4], base_addr_1[4], addr_prefix[8] = {0};

static bool esb_initialized = false;

int esb_initialize(bool tx)
{
	int err;

	struct esb_config config = ESB_DEFAULT_CONFIG;

	if (tx)
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		// config.mode = ESB_MODE_PTX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = 8;
		// config.retransmit_delay = 600;
		config.retransmit_count = 0;
		config.tx_mode = ESB_TXMODE_MANUAL;
		// config.payload_length = 32;
		config.selective_auto_ack = true;
	}
	else
	{
		// config.protocol = ESB_PROTOCOL_ESB_DPL;
		config.mode = ESB_MODE_PRX;
		config.event_handler = event_handler;
		// config.bitrate = ESB_BITRATE_2MBPS;
		// config.crc = ESB_CRC_16BIT;
		config.tx_output_power = 8;
		// config.retransmit_delay = 600;
		// config.retransmit_count = 3;
		// config.tx_mode = ESB_TXMODE_AUTO;
		// config.payload_length = 32;
		config.selective_auto_ack = true;
	}

	// Fast startup mode
	NRF_RADIO->MODECNF0 |= RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;
	// nrf_radio_modecnf0_set(NRF_RADIO, true, 0);

	err = esb_init(&config);

	if (!err)
		esb_set_base_address_0(base_addr_0);

	if (!err)
		esb_set_base_address_1(base_addr_1);

	if (!err)
		esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));

	if (err)
	{
		LOG_ERR("ESB initialization failed: %d", err);
		set_status(SYS_STATUS_CONNECTION_ERROR, true);
		return err;
	}

	esb_initialized = true;
	return 0;
}

inline void esb_set_addr_discovery(void)
{
	memcpy(base_addr_0, discovery_base_addr_0, sizeof(base_addr_0));
	memcpy(base_addr_1, discovery_base_addr_1, sizeof(base_addr_1));
	memcpy(addr_prefix, discovery_addr_prefix, sizeof(addr_prefix));
}

inline void esb_set_addr_paired(void)
{
	// Recreate receiver address
	uint8_t addr_buffer[16] = {0};
	for (int i = 0; i < 4; i++)
	{
		addr_buffer[i] = paired_addr[i + 2];
		addr_buffer[i + 4] = paired_addr[i + 2] + paired_addr[6];
	}
	for (int i = 0; i < 8; i++)
		addr_buffer[i + 8] = paired_addr[7] + i;
	for (int i = 0; i < 16; i++)
	{
		if (addr_buffer[i] == 0x00 || addr_buffer[i] == 0x55 || addr_buffer[i] == 0xAA) // Avoid invalid addresses (see nrf datasheet)
			addr_buffer[i] += 8;
	}
	memcpy(base_addr_0, addr_buffer, sizeof(base_addr_0));
	memcpy(base_addr_1, addr_buffer + 4, sizeof(base_addr_1));
	memcpy(addr_prefix, addr_buffer + 8, sizeof(addr_prefix));
}

static bool esb_paired = false;

void esb_pair(void)
{
	// Read paired address from retained
	// TODO: should pairing data stay within esb?
	memcpy(paired_addr, retained.paired_addr, sizeof(paired_addr));

	if (!paired_addr[0]) // zero, no receiver paired
	{
		LOG_INF("Pairing");
		esb_set_addr_discovery();
		esb_initialize(true);
//		timer_init(); // TODO: shouldn't be here!!!
		tx_payload_pair.noack = false;
		uint64_t *addr = (uint64_t *)NRF_FICR->DEVICEADDR; // Use device address as unique identifier (although it is not actually guaranteed, see datasheet)
		memcpy(&tx_payload_pair.data[2], addr, 6);
		LOG_INF("Device address: %012llX", *addr & 0xFFFFFFFFFFFF);
		uint8_t checksum = crc8_ccitt(0x07, &tx_payload_pair.data[2], 6);
		if (checksum == 0)
			checksum = 8;
		LOG_INF("Checksum: %02X", checksum);
		tx_payload_pair.data[0] = checksum; // Use checksum to make sure packet is for this device
		set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_CONNECTION);
		while (paired_addr[0] != checksum)
		{
			if (paired_addr[0])
			{
				LOG_INF("Incorrect checksum: %02X", paired_addr[0]);
				paired_addr[0] = 0; // Packet not for this device
			}
			esb_flush_rx();
			esb_flush_tx();
			esb_write_payload(&tx_payload_pair); // TODO: Does this still fail after a while?
			esb_start_tx();
			k_msleep(1000);
		}
		set_led(SYS_LED_PATTERN_ONESHOT_COMPLETE, SYS_LED_PRIORITY_CONNECTION);
		LOG_INF("Paired");
		sys_write(PAIRED_ID, retained.paired_addr, paired_addr, sizeof(paired_addr)); // Write new address and tracker id
		esb_disable();
		k_msleep(1600); // wait for led pattern
	}
	LOG_INF("Tracker ID: %u", paired_addr[1]);
	LOG_INF("Receiver Address: %012llX", (*(uint64_t *)&retained.paired_addr[0] >> 16) & 0xFFFFFFFFFFFF);

	connection_set_id(paired_addr[1]);

	esb_set_addr_paired();
	esb_paired = true;
}

void esb_reset_pair(void)
{
	uint8_t empty_addr[8] = {0};
	sys_write(PAIRED_ID, &retained.paired_addr, empty_addr, sizeof(paired_addr)); // write zeroes
	LOG_INF("Pairing data reset");
}

void esb_write(uint8_t *data)
{
	if (!esb_initialized || !esb_paired)
		return;
	tx_payload.noack = false;
	memcpy(tx_payload.data, data, tx_payload.length);
	esb_flush_tx(); // this will clear all transmissions even if they did not complete
	esb_write_payload(&tx_payload); // Add transmission to queue
	send_data = true;
}

bool esb_ready(void)
{
	return esb_initialized && esb_paired;
}
