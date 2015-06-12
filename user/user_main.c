#include "user_interface.h"
#include "user_config.h"
#include "../lib/espuart/uart.h"
#include "../lib/espdht22/dht22.h"

unsigned char *default_certificate;
unsigned int default_certificate_len = 0;
unsigned char *default_private_key;
unsigned int default_private_key_len = 0;

typedef enum {
	WIFI_CONNECTING,
	WIFI_CONNECTING_ERROR,
	WIFI_CONNECTED,
} tConnState;

LOCAL os_timer_t dht22_timer;
LOCAL void ICACHE_FLASH_ATTR setup_wifi_st_mode(void);
static struct ip_info ipConfig;
static ETSTimer WiFiLinker;
static tConnState connState = WIFI_CONNECTING;

const char *WiFiMode[] =
{
		"NULL",		// 0x00
		"STATION",	// 0x01
		"SOFTAP", 	// 0x02
		"STATIONAP"	// 0x03
};

const char *WiFiStatus[] =
{
	    "STATION_IDLE", 			// 0x00
	    "STATION_CONNECTING", 		// 0x01
	    "STATION_WRONG_PASSWORD", 	// 0x02
	    "STATION_NO_AP_FOUND", 		// 0x03
	    "STATION_CONNECT_FAIL", 	// 0x04
	    "STATION_GOT_IP" 			// 0x05
};

LOCAL void ICACHE_FLASH_ATTR hcp_callback(char * response, int http_status, char * full_response)
{
	os_printf("Response:\r\n");
	os_printf("HTTP Status: %d\r\n", http_status);
	os_printf("Body: %s\r\n", response);
}

LOCAL void ICACHE_FLASH_ATTR dht22_cb(void *arg)
{
	static char data[256];
	static char temp[10];
	static char hum[10];
	struct dht_sensor_data* r;
	float lastTemp, lastHum;

	os_timer_disarm(&dht22_timer);
	switch(connState)
	{
		case WIFI_CONNECTED:
			r = DHTRead();
			lastTemp = r->temperature;
			lastHum = r->humidity;
			if(r->success)
			{
					wifi_get_ip_info(STATION_IF, &ipConfig);
					os_sprintf(temp, "%d.%d",(int)(lastTemp),(int)((lastTemp - (int)lastTemp)*100));
					os_sprintf(hum, "%d.%d",(int)(lastHum),(int)((lastHum - (int)lastHum)*100));
					os_printf("Temperature: %s *C, Humidity: %s %%\r\n", temp, hum);

					os_sprintf(data,
							"{\"mode\":\"sync\", \"messageType\":\"1\",\"messages\":[{"
								"\"Humidity\": %s, "
								"\"Temperature\": %s,"
								"\"timestamp\":0"
							"}]}",
							hum, temp);
					hcp_send(HCP_ACCOUNT, HCP_LANDSCAPEHOST, HCP_DEVICEID, HCP_DEVICETOKEN, data, hcp_callback);

			} else {
				os_printf("Error reading temperature and humidity.\r\n");
			}
			break;
		default:
			os_printf("WiFi not connected...\r\n");
	}
	os_timer_setfn(&dht22_timer, (os_timer_func_t *)dht22_cb, (void *)0);
	os_timer_arm(&dht22_timer, DATA_SEND_DELAY, 1);
}

static void ICACHE_FLASH_ATTR wifi_check_ip(void *arg)
{
	os_timer_disarm(&WiFiLinker);
	switch(wifi_station_get_connect_status())
	{
		case STATION_GOT_IP:
			wifi_get_ip_info(STATION_IF, &ipConfig);
			if(ipConfig.ip.addr != 0) {
				connState = WIFI_CONNECTED;
				os_printf("WiFi connected, wait DHT22 timer...\r\n");
			} else {
				connState = WIFI_CONNECTING_ERROR;
				os_printf("WiFi connected, ip.addr is null\r\n");
			}
			break;
		case STATION_WRONG_PASSWORD:
			connState = WIFI_CONNECTING_ERROR;
			os_printf("WiFi connecting error, wrong password\r\n");
			break;
		case STATION_NO_AP_FOUND:
			connState = WIFI_CONNECTING_ERROR;
			os_printf("WiFi connecting error, ap not found\r\n");
			break;
		case STATION_CONNECT_FAIL:
			connState = WIFI_CONNECTING_ERROR;
			os_printf("WiFi connecting fail\r\n");
			break;
		default:
			connState = WIFI_CONNECTING;
			os_printf("WiFi connecting...\r\n");
	}
	os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
	os_timer_arm(&WiFiLinker, 2000, 0);
}


LOCAL void ICACHE_FLASH_ATTR setup_wifi_st_mode(void)
{
	wifi_set_opmode(STATION_MODE);
	struct station_config stconfig;
	wifi_station_disconnect();
	wifi_station_dhcpc_stop();
	if(wifi_station_get_config(&stconfig))
	{
		os_memset(stconfig.ssid, 0, sizeof(stconfig.ssid));
		os_memset(stconfig.password, 0, sizeof(stconfig.password));
		os_sprintf(stconfig.ssid, "%s", WIFI_CLIENTSSID);
		os_sprintf(stconfig.password, "%s", WIFI_CLIENTPASSWORD);
		if(!wifi_station_set_config(&stconfig))
		{
			os_printf("ESP8266 not set station config!\r\n");
		}
	}
	wifi_station_connect();
	wifi_station_dhcpc_start();
	wifi_station_set_auto_connect(1);
	os_printf("ESP8266 in STA mode configured.\r\n");
}

void user_init(void)
{
	// Configure the UART
	uart_init(BIT_RATE_115200,0);
	// Enable system messages
	system_set_os_print(1);
	os_printf("\r\nSDK version:%s\n", system_get_sdk_version());
	os_printf("System init...\r\n");

	os_printf("ESP8266 is %s mode, restarting in %s mode...\r\n", WiFiMode[wifi_get_opmode()], WiFiMode[STATION_MODE]);
	setup_wifi_st_mode();
	if(wifi_get_phy_mode() != PHY_MODE_11N)
		wifi_set_phy_mode(PHY_MODE_11N);
	if(wifi_station_get_auto_connect() == 0)
		wifi_station_set_auto_connect(1);

	// Init DHT22 sensor
	DHTInit(DHT22);

	// Wait for Wi-Fi connection
	os_timer_disarm(&WiFiLinker);
	os_timer_setfn(&WiFiLinker, (os_timer_func_t *)wifi_check_ip, NULL);
	os_timer_arm(&WiFiLinker, 1000, 0);

	// Set up a timer to send the message
	os_timer_disarm(&dht22_timer);
	os_timer_setfn(&dht22_timer, (os_timer_func_t *)dht22_cb, (void *)0);
	os_timer_arm(&dht22_timer, DATA_SEND_DELAY, 1);

	os_printf("System init done.\n");
}

void user_rf_pre_init(void) {}
