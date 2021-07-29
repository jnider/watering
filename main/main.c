#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mdns.h"
#include <time.h>
#include "lwip/apps/sntp.h"
#include <sys/types.h>
#include <esp_ota_ops.h>
#include <esp_https_ota.h>

#include <esp_http_server.h>

#define VER_MAJOR 1
#define VER_MINOR 6
#define MAX_RESPONSE 1024
#define WIFI_CONNECT_TIMEOUT (1000000 * 5)
#define MAX_EVENTS 5
#define MAX_URI_HANDLERS 15
#define OTA_BUF_SIZE 256

#ifndef PIN2STR
#define PIN2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5], (a)[6], (a)[7]
#define PINSTR "%c%c%c%c%c%c%c%c"
#endif

#define BLINK_SLOW 1000000
#define BLINK_FAST 250000

// on the Wemos D1mini board, this is pin D1
#define WATER_PIN GPIO_NUM_5

enum action_types
{
	ACTION_WATER_ON,
	ACTION_WATER_OFF,
	ACTION_ADD_EVENT,
	ACTION_DEL_EVENT,
	ACTION_SET_HOSTNAME,
	ACTION_SET_NTP_HOST,
	ACTION_SET_TIME,
	ACTION_UPDATE_FW,
	MAX_ACTIONS
};

// forward definitions of handlers for uris
esp_err_t handler_index(httpd_req_t *req);
esp_err_t handler_add_event(httpd_req_t *req);
esp_err_t handler_del_event(httpd_req_t *req);
esp_err_t handler_set_wifi(httpd_req_t *req);
esp_err_t handler_set_ntp(httpd_req_t *req);
esp_err_t handler_set_hostname(httpd_req_t *req);
esp_err_t handler_set_time(httpd_req_t *req);
esp_err_t form_hostname(httpd_req_t *req);
esp_err_t form_add_event(httpd_req_t *req);
esp_err_t action_handler_water_on(const char *query);
esp_err_t action_handler_water_off(const char *query);
esp_err_t action_handler_add_event(const char *query);
esp_err_t action_handler_del_event(const char *query);
esp_err_t action_handler_set_hostname(const char *query);
esp_err_t action_handler_update_fw(const char *query);

// There are 3 kinds of events:
// skip > 0: every 'skip' seconds
// skip = 0, days = 0: special case meaning 'every day'
// skip = 0, days != 0: on the specified days of the week
typedef struct water_event
{
	bool enabled;			// is the event valid
	uint8_t hour;			// starting hour
	uint8_t minute;		// starting minute
	uint8_t skip;			// how many seconds to skip before recurrance (0 = read 'days')
	uint8_t days;			// bitmap of specific days to execute on
	uint32_t duration;	// how many seconds to leave the tap open
} water_event;

typedef struct program_state
{
	int led;			// the blue indicator led
	bool water_on;		// is the water circuit supposed to be on or not
	bool internet;		// is there a connection to the Internet?
	time_t last_watering;
	int last_duration;
	water_event schedule[MAX_EVENTS];
} program_state;

struct action
{
	char name[16];
	esp_err_t (*handler)(const char *query);
};

static const char *day_str[] =
{
	"Sunday",
	"Monday",
	"Tuesday",
	"Wednesday",
	"Thursday",
	"Friday",
	"Saturday"
};

static char ntp_server[64] = "pool.ntp.org";
static char hostname[32] = "default";
static const char *TAG="APP";
static const char nvs_namespace[] = "ns_wifi";
static esp_wps_config_t wps_config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
static esp_timer_handle_t blink_timer;
static esp_timer_handle_t connect_timer;
static esp_timer_handle_t schedule_timer;
static esp_timer_handle_t water_timer;
static esp_timer_handle_t reboot_timer;
static program_state state = 
{
	.led = 0,
	.water_on = false,
	.last_watering = 0,
	.last_duration = 0,
	.internet = false,
};
static struct action actions[MAX_ACTIONS] =
{
	{
		.name = "water_on",
		.handler = action_handler_water_on
	},
	{
		.name = "water_off",
		.handler = action_handler_water_off
	},
	{
		.name = "add_event",
		.handler = action_handler_add_event
	},
	{
		.name = "del_event",
		.handler = action_handler_del_event
	},
	{
		.name = "sethostname",
		.handler = action_handler_set_hostname
	},
	{
		.name = "update_fw",
		.handler = action_handler_update_fw
	}
};

httpd_uri_t uris[] = {
{
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = handler_index,
    .user_ctx  = ""
},
{
    .uri       = "/index.html",
    .method    = HTTP_GET,
    .handler   = handler_index,
    .user_ctx  = ""
},
{
    .uri       = "/host.html",
    .method    = HTTP_GET,
    .handler   = form_hostname,
    .user_ctx  = ""
},
{
    .uri       = "/add_event",
    .method    = HTTP_GET,
    .handler   = form_add_event,
    .user_ctx  = ""
},
{
    .uri       = "/config/set_wifi",
    .method    = HTTP_POST,
    .handler   = handler_set_wifi,
    .user_ctx  = ""
},
{
    .uri       = "/config/set_ntp",
    .method    = HTTP_POST,
    .handler   = handler_set_ntp,
    .user_ctx  = ""
},
{
    .uri       = "/config/set_hostname",
    .method    = HTTP_POST,
    .handler   = handler_set_hostname,
    .user_ctx  = ""
},
{
    .uri       = "/config/set_time",
    .method    = HTTP_POST,
    .handler   = handler_set_time,
    .user_ctx  = ""
},
};

/*
	Taken from https://stackoverflow.com/questions/2673207/c-c-url-decode-library
	This should be in a library somewhere
*/
void urldecode2(char *dst, const char *src)
{
	char a, b;
	while (*src)
	{
		if ((*src == '%') &&
			((a = src[1]) && (b = src[2])) &&
			(isxdigit(a) && isxdigit(b)))
		{
			if (a >= 'a')
				a -= 'a'-'A';
			if (a >= 'A')
				a -= ('A' - 10);
			else
				a -= '0';
			if (b >= 'a')
				b -= 'a'-'A';
			if (b >= 'A')
				b -= ('A' - 10);
			else
				b -= '0';
			*dst++ = 16*a+b;
			src+=3;
		}
		else if (*src == '+')
		{
			*dst++ = ' ';
			src++;
		}
		else
		{
			*dst++ = *src++;
		}
	}
	*dst++ = '\0';
}

void toggle_led(void)
{
	state.led = !state.led;
	gpio_set_level(GPIO_NUM_2, state.led);
}

void led_off(void)
{
	state.led = 1;
	gpio_set_level(GPIO_NUM_2, state.led);
}

void blink_callback(void *arg)
{
	toggle_led();
}

void blink_start(uint64_t rate)
{
	esp_timer_start_periodic(blink_timer, rate);
}

void blink_stop(void)
{
	esp_timer_stop(blink_timer);
}

static void turn_water_on(void)
{
	struct tm timeinfo = { 0 };
	char timestr[64];

	gpio_set_level(WATER_PIN, 1);
	state.water_on = true;

	// record the time the water started
	time(&state.last_watering);
	localtime_r(&state.last_watering, &timeinfo);
	strftime(timestr, sizeof(timestr), "%c", &timeinfo);
	ESP_LOGI(TAG, "Water on at %s", timestr);
}

static void turn_water_off(void)
{
	struct tm timeinfo = { 0 };
	time_t now = 0;
	char timestr[64];

	gpio_set_level(WATER_PIN, 0);
	state.water_on = false;

	time(&now);
	localtime_r(&now, &timeinfo);
	strftime(timestr, sizeof(timestr), "%c", &timeinfo);


	// record the duration the water was on
	state.last_duration = now - state.last_watering;

	ESP_LOGI(TAG, "Water off at %s", timestr);
}

int add_water_event(water_event *new_event)
{
	nvs_handle nvs;

	for (uint8_t evt = 0; evt < MAX_EVENTS; evt++)
	{
		if (!state.schedule[evt].enabled)
		{
			ESP_LOGI(TAG, "Adding event[%u] @%02u:%02u skip=%u days=%u duration=%u", evt,
				new_event->hour, new_event->minute, new_event->skip, new_event->days, new_event->duration);
			memcpy(&state.schedule[evt], new_event, sizeof(water_event));
			state.schedule[evt].enabled = true;

			// save it in NVS
			if (nvs_open(nvs_namespace, NVS_READWRITE, &nvs) == ESP_OK)
			{
				char event_name[10];
				sprintf(event_name, "evt%02u", evt);
				nvs_set_blob(nvs, event_name, &state.schedule[evt], sizeof(water_event));
				nvs_close(nvs);
			}

			return 0;
		}
	}

	// no empty slots
	return -1;
}

int del_water_event(uint8_t evt)
{
	nvs_handle nvs;

	if (evt < MAX_EVENTS)
	{
		state.schedule[evt].enabled = false;

		// save it in NVS
		if (nvs_open(nvs_namespace, NVS_READWRITE, &nvs) == ESP_OK)
		{
			char event_name[10];
			sprintf(event_name, "evt%02u", evt);
			nvs_set_blob(nvs, event_name, &state.schedule[evt], sizeof(water_event));
			nvs_close(nvs);
		}
		return 0;
	}

	return -1;
}

esp_err_t action_handler_water_on(const char *query)
{
	turn_water_on();
	return ESP_OK;
}

esp_err_t action_handler_water_off(const char *query)
{
	turn_water_off();
	return ESP_OK;
}

esp_err_t action_handler_add_event(const char *query)
{
	water_event event;
	char value[64];

	memset(&event, 0, sizeof(water_event));

	ESP_LOGI(TAG, "Adding event\n");
	if (httpd_query_key_value(query, "time", value, 8) == ESP_OK)
	{
		char time[64];
		urldecode2(time, value);
		ESP_LOGI(TAG, "Time: %s\n", time);
		event.hour = atoi(time);
		event.minute = atoi(time+3);
		ESP_LOGI(TAG, "Time: %u:%u\n", event.hour, event.minute);
	}

	if (httpd_query_key_value(query, "skip", value, 4) == ESP_OK)
		event.skip = atoi(value);

	if (httpd_query_key_value(query, "d0", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 0;
	if (httpd_query_key_value(query, "d1", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 1;
	if (httpd_query_key_value(query, "d2", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 2;
	if (httpd_query_key_value(query, "d3", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 3;
	if (httpd_query_key_value(query, "d4", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 4;
	if (httpd_query_key_value(query, "d5", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 5;
	if (httpd_query_key_value(query, "d6", value, 3) == ESP_OK && strcmp(value, "on") == 0)
		event.days |= 1 << 6;

	if (httpd_query_key_value(query, "duration", value, 5) == ESP_OK)
		event.duration = atoi(value);

	// add the event
	if (add_water_event(&event) == 0)
		return ESP_OK;

	return ESP_FAIL;
}

esp_err_t action_handler_del_event(const char *query)
{
	char value[10];
	uint8_t index;

	ESP_LOGI(TAG, "Delete event");
	if (httpd_query_key_value(query, "index", value, 3) == ESP_OK)
	{
		index = atoi(value);
		if (index < MAX_EVENTS)
			del_water_event(index);
	}

	return ESP_OK;
}

esp_err_t action_handler_set_hostname(const char *query)
{
	return ESP_OK;
}

esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
	// don't really need to handle any events yet
	return ESP_OK;
}

esp_err_t action_handler_update_fw(const char *query)
{
	const esp_partition_t *update_partition = NULL;
	esp_ota_handle_t update_handle = 0;
	char *upgrade_data_buf;
	int binary_file_len = 0;
	esp_http_client_handle_t client;
	esp_http_client_config_t config =
	{
		.url = "http://192.168.20.30/water.bin",
		.event_handler = http_client_event_handler,
	};


	client = esp_http_client_init(&config);
	if (!client)
	{
		ESP_LOGE(TAG, "Error initializing http client");
		return ESP_FAIL;
	}

	if (esp_http_client_open(client, 0) != ESP_OK)
	{
		ESP_LOGE(TAG, "Error opening http client");
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	esp_http_client_fetch_headers(client);

	update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition)
	{
		ESP_LOGE(TAG, "Can't find partition for update");
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Writing to partition type %i at offset 0x%x",
		update_partition->subtype, update_partition->address);

	if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "Can't start upgrade");
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "allocating %u bytes", OTA_BUF_SIZE);
	upgrade_data_buf = (char*)malloc(OTA_BUF_SIZE);
	if (!upgrade_data_buf)
	{
		ESP_LOGE(TAG, "Can't allocate memory");
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Updating");
	binary_file_len = 0;
	while (1)
	{
		int data_read = esp_http_client_read(client, upgrade_data_buf, OTA_BUF_SIZE);
		if (data_read == 0)
			break;

		if (data_read < 0)
		{
			ESP_LOGE(TAG, "Error reading socket");
			free(upgrade_data_buf);
			esp_http_client_cleanup(client);
			return ESP_FAIL;
		}

		if (esp_ota_write(update_handle, upgrade_data_buf, data_read) != ESP_OK)
		{
			ESP_LOGE(TAG, "Error writing fw");
			free(upgrade_data_buf);
			esp_http_client_cleanup(client);
			return ESP_FAIL;
		}
		binary_file_len += data_read;
	}
	free(upgrade_data_buf);
	esp_http_client_cleanup(client);

	if (esp_ota_end(update_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "Upgrade failed");
		return ESP_FAIL;
	}

	if (esp_ota_set_boot_partition(update_partition) != ESP_OK)
	{
		ESP_LOGE(TAG, "Can't set boot partition");
		return ESP_FAIL;
	}

	// schedule reboot to occur after we make sure it's safe
	ESP_LOGI(TAG, "Update done - rebooting");
	esp_timer_start_once(reboot_timer, 1000000);
	return ESP_OK;
}

/*
	Time to turn off the water
*/
void water_callback(void *arg)
{
	turn_water_off();
}

/*
	A reboot is needed
*/
void reboot_callback(void *arg)
{
	ESP_LOGI(TAG, "Rebooting");
	turn_water_off();
	esp_restart();
}

/*
	Call this periodically to perform various tasks
	(turn on the water, update the date/time, etc.)
*/
void scheduler(void *arg)
{
	time_t now = 0;
	struct tm timeinfo = { 0 };
	char line[100];
	uint8_t evt;

	time(&now);
	localtime_r(&now, &timeinfo);
	strftime(line, sizeof(line), "%c", &timeinfo);
	//ESP_LOGI(TAG, "Scheduler @ %s", line);

	// iterate through all events
	for (evt = 0; evt < MAX_EVENTS; evt++)
	{
		water_event *event = &state.schedule[evt];
		if (event->enabled)
		{
			if (event->skip > 0)
			{
				if (now % event->skip == 0)
				{
					turn_water_on();
					esp_timer_start_once(water_timer, event->duration * 1000000);
				}
			}
			else
			{
				if (event->days == 0 || event->days & (1 << timeinfo.tm_wday))
				{
					if (timeinfo.tm_hour == event->hour && timeinfo.tm_min == event->minute)
					{
						turn_water_on();
						esp_timer_start_once(water_timer, event->duration * 1000000);
					}
				}
			}
		}
	}
}

void no_connect_callback(void *arg)
{
	ESP_LOGI(TAG, "Can't connect to previous AP - starting WPS");
	ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
	ESP_ERROR_CHECK(esp_wifi_wps_start(0));
	ESP_LOGI(TAG, "WPS waiting");

	// blink slowly to show that WPS is waiting
	blink_start(BLINK_SLOW);
}

esp_err_t handler_help(httpd_req_t *req)
{
	char line[100];
	char resp_str[MAX_RESPONSE];
	resp_str[0] = 0;

	strcat(resp_str, "<html><title>Watering System - Help</title>\n<body>\n");
	snprintf(line, 100, "<h1>Joel's Watering System v%u.%u</h1>\n", VER_MAJOR, VER_MINOR);
	strcat(resp_str, line);
	strcat(resp_str, "<h2>Command Help</h2><table><tr><td>Action<td>Parameters<td>Description<td>Example</tr>\n");
	strcat(resp_str, "<tr><td>water_on<td><td>Turn water on now<td>http://192.168.1.1/?action=water_on</tr>\n");
	strcat(resp_str, "<tr><td>water_off<td><td>Turn water off now<td>http://192.168.1.1/?action=water_off</tr>\n");
	strcat(resp_str, "<tr><td>add_event<td>time=[hh:mm], d0..d6=[on|off], duration=[secs]<td>Schedule a new watering event<td>http://192.168.1.1/?action=add_event&time=14%0e30&d1=on&d3=on&duration=60</tr>\n");
	strcat(resp_str, "<tr><td><td>time=[hh:mm], skip=[secs], duration=[secs]<td>Schedule a new watering event, repeating every N seconds<td>http://192.168.1.1/?action=add_event&time=14%0e30&skip=3600&duration=15</tr>\n");
	strcat(resp_str, "<tr><td>del_event<td>index=<event><td>Delete an existing event<td>\n");
	strcat(resp_str, "<a href=\"/\">Return to main page</a>\n");

	strcat(resp_str, "<h1>Add Event</h1>\n<form action=\"/\" method=\"PUT\">\n");
	strcat(resp_str, "<br><input type=\"hidden\" name=\"action\" value=\"add_event\">\n");
	strcat(resp_str, "<table><tr><td>Turn on at<td><input type=\"time\" name=\"time\"></tr>\n");
	//strcat(resp_str, "<tr><td>Every<td><input type=\"number\" name=\"skip\" width=5>seconds</tr>\n");
	strcat(resp_str, "<tr><td>On these days<td><input type=\"checkbox\" name=\"d0\">Sunday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d1\">Monday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d2\">Tuesday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d3\">Wednesday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d4\">Thursday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d5\">Friday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d6\">Saturday</tr>\n");
	strcat(resp_str, "<tr><td>For<td><input type=\"number\" name=\"duration\" maxlength=4 max=3600> seconds</tr></table>\n");
	return httpd_resp_send(req, resp_str, strlen(resp_str));
}

esp_err_t handler_index(httpd_req_t *req)
{
	time_t now = 0;
	struct tm timeinfo = { 0 };
	wifi_config_t wifi_config;
	char line[100];
	char resp_str[MAX_RESPONSE];
	char query[256];
	uint8_t num_events = 0;
	uint8_t evt;
	uint8_t mac[7];
	wifi_ap_record_t ap_info;
	esp_err_t err;
	bool command = false;
	uint8_t action_idx;

	// find out what the users wants to do
	if (httpd_req_get_url_query_str(req, query, 256) == ESP_OK)
	{
		ESP_LOGI(TAG, "Query: %s", query);
		if (httpd_query_key_value(query, "action", line, sizeof(line)) == ESP_OK)
		{
			for (action_idx = 0; action_idx < MAX_ACTIONS; action_idx++)
			{
				if (strcmp(line, actions[action_idx].name) == 0)
				{
					err = actions[action_idx].handler(query);
					command = true;
					break;
				}
			}

			if (action_idx == MAX_ACTIONS)
			{
				// user needs help
				return handler_help(req);
			}
		}
	}

	esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config);
	if (esp_base_mac_addr_get(mac) == ESP_ERR_INVALID_MAC)
		esp_efuse_mac_get_default(mac);
	esp_wifi_sta_get_ap_info(&ap_info);

	time(&now);
	localtime_r(&now, &timeinfo);
	strftime(line, sizeof(line), "%c", &timeinfo);

	resp_str[0] = 0;
	strcat(resp_str, "<html><title>Watering System</title>\n<body>\n");
	snprintf(line, 100, "<h1>Joel's Watering System v%u.%u</h1>\n", VER_MAJOR, VER_MINOR);
	strcat(resp_str, line);
	strcat(resp_str, "<h2>Status</h2><table><tr><td>Time<td>\n");
	strftime(line, sizeof(line), "%c</tr>", &timeinfo);
	strcat(resp_str, line);

	snprintf(line, 100, "<td>Water<td>%s</tr>\n", state.water_on ? "On" : "Off");
	strcat(resp_str, line);

	if (state.last_watering)
	{
		snprintf(line, 100, "<td>Last watering at<td>%s for %i minute%s %i second%s</tr>\n",
			ctime(&state.last_watering), state.last_duration / 60,
			(state.last_duration / 60 == 1) ? "" : "s",
			state.last_duration % 60,
			(state.last_duration % 60 == 1) ? "" : "s");
		strcat(resp_str, line);
	}

	// print the status if we executed a command
	if (command)
	{
		if (err == ESP_OK)
			snprintf(line, 100, "<tr><td><td>%s command ok</tr>\n", actions[action_idx].name);
		else
			snprintf(line, 100, "<tr><td><td>%s command failed: %u</tr>\n", actions[action_idx].name, err);
	}
	strcat(resp_str, "</table>\n");

	strcat(resp_str, "<h2>Schedule</h2>\n");
	for (evt = 0; evt < MAX_EVENTS; evt++)
	{
		bool first_day = true;
		water_event *event = &state.schedule[evt];

		if (event->enabled)
		{
			snprintf(line, 100, "[%u] ", evt);
			strcat(resp_str, line);

			num_events++;
			if (event->skip)
			{
				snprintf(line, 100, "Every %u seconds at %02u:%02u for %u seconds<br>\n",
					event->skip+1, event->hour, event->minute, event->duration);
				strcat(resp_str, line);
			}
			else
			{
				if (event->days == 0)
				{
					snprintf(line, 100, "Every day at %02u:%02u for %u seconds<br>\n",
						event->hour, event->minute, event->duration);
					strcat(resp_str, line);
				}
				else
				{
					strcat(resp_str, "Every week on ");
					for (uint8_t day = 0; day < 7; day++)
					{
						if (event->days & (1 << day))
						{
							if (first_day)
								first_day = false;
							else
								strcat(resp_str, ", ");
							strcat(resp_str, day_str[day]);
						}
					}
					snprintf(line, 100, " at %02u:%02u for %u seconds <a href=/?action=del_event&index=%u>[-]</a><br>\n",
						event->hour, event->minute, event->duration, evt);
					strcat(resp_str, line);
				}
			}
		}
	}

	if (num_events == 0)
		strcat(resp_str, "No scheduled events<br>");
	if (num_events < MAX_EVENTS)
		strcat(resp_str, "<a href=/add_event>[+] Add event</a><br>\n");

	strcat(resp_str, "<h2>Networking</h2>\n<table>");
	snprintf(line, 100, "<tr><td>Access Point<td>%s</tr>\n", wifi_config.sta.ssid);
	strcat(resp_str, line);
	snprintf(line, 100, "<tr><td>Time Server<td>%s</tr>\n", ntp_server);
	strcat(resp_str, line);
	snprintf(line, 100, "<tr><td>Hostname<td>%s</tr>\n", hostname);
	strcat(resp_str, line);
	snprintf(line, 100, "<tr><td>MAC<td>%02x:%02x:%02x:%02x:%02x:%02x</tr>\n",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	strcat(resp_str, line);
	snprintf(line, 100, "<tr><td>Signal strength<td>%i dBm</tr>", ap_info.rssi);
	strcat(resp_str, line);
	snprintf(line, 100, "<tr><td>Internet<td>%s</tr>\n", state.internet ? "connected" : "disconnected");
	strcat(resp_str, line);
	strcat(resp_str, "</table>\n");

	strcat(resp_str, "<h2>Control</h2>\n");
	if (state.water_on)
		snprintf(line, 100, "<a href=\"/?action=water_off\">Water Off</a><br>\n");
	else
		snprintf(line, 100, "<a href=\"/?action=water_on\">Water On</a><br>\n");
	strcat(resp_str, line);
	strcat(resp_str, "<a href=\"/?action=update_fw\">Update Firmware</a><br>\n");
	strcat(resp_str, "</body></html>");
	ESP_LOGI(TAG, "Length %u", strlen(resp_str));

	httpd_resp_send(req, resp_str, strlen(resp_str));

	return ESP_OK;
}

esp_err_t form_hostname(httpd_req_t *req)
{
	char line[100];
	char resp_str[MAX_RESPONSE];
	resp_str[0] = 0;

	strcat(resp_str, "<html><title>Watering System</title>\n<body>\n");
	strcat(resp_str, "<h1>Set Hostname</h1>\n<form action=\"/config/set_hostname\" method=\"POST\">\n");
	snprintf(line, 100, "<input type=\"text\" id=\"host\" value=\"%s\"><br>", hostname);
	strcat(resp_str, line);
	strcat(resp_str, "<br><input type=\"submit\" value=\"Update\">\n");
	strcat(resp_str, "</form></body></html>");

	return httpd_resp_send(req, resp_str, strlen(resp_str));
}

esp_err_t form_add_event(httpd_req_t *req)
{
	char resp_str[MAX_RESPONSE];
	resp_str[0] = 0;

	strcat(resp_str, "<html><title>Watering System</title>\n<body>\n");
	strcat(resp_str, "<h1>Add Event</h1>\n<form action=\"/\" method=\"PUT\">\n");
	strcat(resp_str, "<br><input type=\"hidden\" name=\"action\" value=\"add_event\">\n");
	strcat(resp_str, "<table><tr><td>Turn on at<td><input type=\"time\" name=\"time\"></tr>\n");
	//strcat(resp_str, "<tr><td>Every<td><input type=\"number\" name=\"skip\" width=5>seconds</tr>\n");
	strcat(resp_str, "<tr><td>On these days<td><input type=\"checkbox\" name=\"d0\">Sunday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d1\">Monday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d2\">Tuesday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d3\">Wednesday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d4\">Thursday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d5\">Friday</tr>\n");
	strcat(resp_str, "<tr><td><td><input type=\"checkbox\" name=\"d6\">Saturday</tr>\n");
	strcat(resp_str, "<tr><td>For<td><input type=\"number\" name=\"duration\" maxlength=4 max=3600> seconds</tr></table>\n");
	strcat(resp_str, "<input type=\"submit\" value=\"Add\">\n");
	strcat(resp_str, "</form></body></html>");

	return httpd_resp_send(req, resp_str, strlen(resp_str));
}

/*
	Set time manually
*/
esp_err_t handler_set_time(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	struct tm timeinfo = { 0 };
	struct timeval newtime;
	char strftime_buf[64];

	// parse the parameters to get the new time
	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		if (httpd_query_key_value(query, "sec", value, 3) == ESP_OK)
			timeinfo.tm_sec = atoi(value);
		else
			return httpd_resp_send_500(req);
		if (httpd_query_key_value(query, "min", value, 3) == ESP_OK)
			timeinfo.tm_min = atoi(value);
		else
			return httpd_resp_send_500(req);
		if (httpd_query_key_value(query, "hour", value, 3) == ESP_OK)
			timeinfo.tm_hour = atoi(value);
		else
			return httpd_resp_send_500(req);
		if (httpd_query_key_value(query, "day", value, 3) == ESP_OK)
			timeinfo.tm_mday = atoi(value);
		else
			return httpd_resp_send_500(req);

		// the month is 0-based so subtract 1
		if (httpd_query_key_value(query, "mon", value, 3) == ESP_OK)
			timeinfo.tm_mon = atoi(value) - 1;
		else
			return httpd_resp_send_500(req);

		// the year counts from 1900
		if (httpd_query_key_value(query, "year", value, 5) == ESP_OK)
			timeinfo.tm_year = atoi(value) - 1900;
		else
			return httpd_resp_send_500(req);
	}
	else
		ESP_LOGI(TAG, "can't find query");

	newtime.tv_usec = 0;
	newtime.tv_sec = mktime(&timeinfo);
	settimeofday(&newtime, NULL);
	strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
	ESP_LOGI(TAG, "Time set to %s", strftime_buf);

	sprintf(resp_str, "OK\n");
	httpd_resp_send(req, resp_str, strlen(resp_str));

	return ESP_OK;
}

esp_err_t handler_del_event(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	uint8_t evt=MAX_EVENTS;	// event index

	// parse the parameters to get the new time - all fields are mandatory
	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		if (httpd_query_key_value(query, "event", value, 3) == ESP_OK)
			evt = atoi(value);
	}

	if (del_water_event(evt) == 0)
	{
		sprintf(resp_str, "OK\n");
		return httpd_resp_send(req, resp_str, strlen(resp_str));
	}

	return httpd_resp_send_500(req);
}

esp_err_t handler_add_event(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	water_event event;

	memset(&event, 0, sizeof(water_event));

	// parse the parameters to get the new time - all fields are mandatory
	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		if (httpd_query_key_value(query, "hour", value, 3) == ESP_OK)
			event.hour = atoi(value);

		if (httpd_query_key_value(query, "minute", value, 3) == ESP_OK)
			event.minute = atoi(value);

		if (httpd_query_key_value(query, "skip", value, 4) == ESP_OK)
			event.skip = atoi(value);

		if (httpd_query_key_value(query, "days", value, 4) == ESP_OK)
			event.days = atoi(value);

		if (httpd_query_key_value(query, "duration", value, 6) == ESP_OK)
			event.duration = atoi(value);
	}

	// add the event
	if (add_water_event(&event) == 0)
	{
		sprintf(resp_str, "OK\n");
		return httpd_resp_send(req, resp_str, strlen(resp_str));
	}

	return httpd_resp_send_500(req);
}

esp_err_t handler_set_wifi(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	bool new_ssid = false;
	wifi_config_t wifi_config;

	esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config);

	// parse the parameters to find the new ssid name
	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		if (httpd_query_key_value(query, "ssid", value, 32) == ESP_OK)
		{
			//ESP_LOGI(TAG, "ssid: %s", urldecode(value));
			strncpy((char*)wifi_config.sta.ssid, value, 32);
			new_ssid = true;
		}
		if (httpd_query_key_value(query, "pw", value, 64) == ESP_OK)
		{
			strncpy((char*)wifi_config.sta.password, value, 64);
			//ESP_LOGI(TAG, "pw: %s", urldecode(value));
		}
	}
	else
		ESP_LOGI(TAG, "can't find query");


	sprintf(resp_str, "OK\n");
	httpd_resp_send(req, resp_str, strlen(resp_str));

	if (new_ssid)
	{
		sleep(3);
		ESP_LOGI(TAG, "Connecting to AP: %s with password %s", wifi_config.sta.ssid, wifi_config.sta.password);
		esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
		esp_wifi_disconnect();
		//esp_timer_start_once(reboot_timer, 100000);
	}
	return ESP_OK;
}

esp_err_t handler_set_ntp(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	nvs_handle nvs;

	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		if ((httpd_query_key_value(query, "ntp", value, 64) == ESP_OK)
		&& (nvs_open(nvs_namespace, NVS_READWRITE, &nvs) == ESP_OK))
		{
			size_t length;
			char old_value[64];

			// check if the value has changed
			length = 64;
			if (nvs_get_str(nvs, "ntp", old_value, &length) != ESP_OK || strcmp(old_value, value) != 0)
			{
				nvs_set_str(nvs, "ntp", value);
				strcpy(ntp_server, value);
				ESP_LOGI(TAG, "NTP server: %s\n", value);
			}
			nvs_close(nvs);
		}
	}

	// respond to client
	sprintf(resp_str, "OK\n");
	httpd_resp_send(req, resp_str, strlen(resp_str));
	return ESP_OK;
}

esp_err_t handler_set_hostname(httpd_req_t *req)
{
	char resp_str[14];
	char query[256];
	char value[64];
	nvs_handle nvs;

	if (httpd_req_recv(req, query, 256) > 0)
	{
		query[req->content_len] = 0;
		ESP_LOGI(TAG, "Query: %s", query);
		if ((httpd_query_key_value(query, "host", value, 64) == ESP_OK)
		&& (nvs_open(nvs_namespace, NVS_READWRITE, &nvs) == ESP_OK))
		{
			size_t length;
			char old_value[32];

			// check if the value has changed
			length = 32;
			if (nvs_get_str(nvs, "host", old_value, &length) != ESP_OK || strcmp(old_value, value) != 0)
			{
				nvs_set_str(nvs, "host", value);
				strcpy(hostname, value);
				ESP_LOGI(TAG, "New hostname: %s", value);
			}
			nvs_close(nvs);
		}
	}

	// respond to client
	sprintf(resp_str, "OK\n");
	httpd_resp_send(req, resp_str, strlen(resp_str));
	return ESP_OK;
}

// Set time from NTP server
static void obtain_time(void)
{
	time_t now = 0;
	struct tm timeinfo = { 0 };
	int retry = 0;
	const int retry_count = 10;
	char strftime_buf[64];

	ESP_LOGI(TAG, "Syncing NTP time from %s", ntp_server);
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, ntp_server);
	sntp_init();

	while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
	{
		ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
		sleep(1);
		time(&now);
		localtime_r(&now, &timeinfo);
	}

	if (retry < retry_count)
	{
		state.internet = true;
		strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
		ESP_LOGI(TAG, "Time is: %s", strftime_buf);
	}
}

httpd_handle_t start_webserver(void)
{
	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();

	// Start the httpd server
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	config.max_uri_handlers = MAX_URI_HANDLERS;
	if (httpd_start(&server, &config) == ESP_OK)
	{
		// Set URI handlers
		for (int i=0; i < sizeof(uris)/sizeof(httpd_uri_t); i++)
		{
			ESP_LOGI(TAG, "Registering URI handler %s", uris[i].uri);
			httpd_register_uri_handler(server, &uris[i]);
		}
		return server;
	}

	ESP_LOGI(TAG, "Error starting server!");
	return NULL;
}

static httpd_handle_t server = NULL;

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
	httpd_handle_t* server = (httpd_handle_t*) arg;
	if (*server)
	{
		ESP_LOGI(TAG, "Stopping webserver");
		httpd_stop(server);
		*server = NULL;
	}
}

static void on_ip_connect(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    httpd_handle_t* server = (httpd_handle_t*) arg;

    ESP_LOGI(TAG, "got ip: %s", ip4addr_ntoa(&event->ip_info.ip));

	// stop blinking - we are connected
	blink_stop();
	led_off();

    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }

	if (server)
	{
		ESP_LOGI(TAG, "starting mdnsd water service");
		mdns_instance_name_set("water");
		mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
	}

	obtain_time();
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
	//nvs_handle nvs;
	wifi_config_t wifi_config;
	esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config);

	switch (event_id)
	{
	case WIFI_EVENT_STA_START:
		ESP_LOGI(TAG, "Reconnecting to AP: %s with password %s", wifi_config.sta.ssid, wifi_config.sta.password);
		blink_start(BLINK_FAST);
		if (esp_wifi_connect() == ESP_OK)
		{
			esp_timer_start_once(connect_timer, WIFI_CONNECT_TIMEOUT);
			break;
		}

		// start WPS - wait for SSID and password
		ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
		ESP_ERROR_CHECK(esp_wifi_wps_start(0));
		ESP_LOGI(TAG, "WPS waiting");
		blink_start(BLINK_SLOW);
		break;

	case WIFI_EVENT_STA_CONNECTED:
		esp_timer_stop(connect_timer);
		ESP_LOGI(TAG, "WIFI connected to %s\n", wifi_config.sta.ssid);
		break;

	case WIFI_EVENT_STA_DISCONNECTED:
		{
			system_event_sta_disconnected_t *event = (system_event_sta_disconnected_t *)event_data;

			ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
			if (event->reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT)
			{
				/* Switch to 802.11 bgn mode */
				esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
			}

			blink_start(BLINK_FAST);
			ESP_ERROR_CHECK(esp_wifi_connect());
	   }
		break;

	case WIFI_EVENT_STA_WPS_ER_SUCCESS:
		ESP_LOGI(TAG, "WPS got SSID and password");
		blink_start(BLINK_FAST);
		/* esp_wifi_wps_start() only gets ssid & password, so call esp_wifi_connect() here. */
		ESP_ERROR_CHECK(esp_wifi_wps_disable());
		ESP_ERROR_CHECK(esp_wifi_connect());
		break;

	case WIFI_EVENT_STA_WPS_ER_FAILED:
		ESP_LOGI(TAG, "WIFI_EVENT_STA_WPS_ER_FAILED");
		ESP_ERROR_CHECK(esp_wifi_wps_disable());
		ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
		ESP_ERROR_CHECK(esp_wifi_wps_start(0));
		blink_start(BLINK_SLOW);
		break;

	case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
		ESP_ERROR_CHECK(esp_wifi_wps_disable());
		ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
		ESP_ERROR_CHECK(esp_wifi_wps_start(0));
		ESP_LOGI(TAG, "WPS waiting");
		blink_start(BLINK_SLOW);
		break;

        case WIFI_EVENT_STA_WPS_ER_PIN:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_WPS_ER_PIN");
            /* display the PIN code */
            wifi_event_sta_wps_er_pin_t* event = (wifi_event_sta_wps_er_pin_t*) event_data;
            ESP_LOGI(TAG, "WPS_PIN = " PINSTR, PIN2STR(event->pin_code));
            break;
        default:
            break;
    }
}

void app_main()
{
	uint8_t mac[7];
	nvs_handle nvs;

	const esp_timer_create_args_t blink_timer_args = {
		.callback = blink_callback,
		.arg = &blink_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	const esp_timer_create_args_t connect_timer_args = {
		.callback = no_connect_callback,
		.arg = &connect_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	const esp_timer_create_args_t schedule_timer_args = {
		.callback = scheduler,
		.arg = &schedule_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	const esp_timer_create_args_t water_timer_args = {
		.callback = water_callback,
		.arg = &water_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	const esp_timer_create_args_t reboot_timer_args = {
		.callback = reboot_callback,
		.arg = &reboot_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	// make sure all events are off until they are programmed
	for (uint8_t evt = 0; evt < MAX_EVENTS; evt++)
		state.schedule[evt].enabled = false;

	// set up wifi configuration
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	tcpip_adapter_init();

	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
	gpio_set_direction(WATER_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(WATER_PIN, 0);

	// set up software
	esp_timer_create(&blink_timer_args, &blink_timer);
	esp_timer_create(&connect_timer_args, &connect_timer);
	esp_timer_create(&schedule_timer_args, &schedule_timer);
	esp_timer_create(&water_timer_args, &water_timer);
	esp_timer_create(&reboot_timer_args, &reboot_timer);

   ESP_ERROR_CHECK(nvs_flash_init());
   ESP_ERROR_CHECK(esp_netif_init());
   ESP_ERROR_CHECK(esp_event_loop_create_default());

	ESP_LOGI(TAG, "Watering System v%u.%u", VER_MAJOR, VER_MINOR);

	if (esp_base_mac_addr_get(mac) == ESP_ERR_INVALID_MAC)
	{
		esp_efuse_mac_get_default(mac);
		esp_base_mac_addr_set(mac);
	}

	// enable the wifi
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	// set to 'station' mode (client)
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

	// read the stored variables from flash
	if (nvs_open(nvs_namespace, NVS_READONLY, &nvs) == ESP_OK)
	{
		char value[64];
		size_t length;

		// NTP server name
		length = 64;
		if (nvs_get_str(nvs, "ntp0", value, &length) == ESP_OK)
		{
			strncpy(ntp_server, value, 64);
		}

		// host name
		length = 32;
		if (nvs_get_str(nvs, "host", value, &length) == ESP_OK)
		{
			strncpy(hostname, value, 32);
		}

		// scheduled events
		for (uint8_t evt = 0; evt < MAX_EVENTS; evt++)
		{
			char event_name[10];
			sprintf(event_name, "evt%02u", evt);
			length = sizeof(water_event);
			nvs_get_blob(nvs, event_name, &state.schedule[evt], &length);
		}

		nvs_close(nvs);
	}

	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_connect, &server));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, &server));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_ERROR_CHECK(mdns_init());

	ESP_LOGI(TAG, "Using NTP server %s", ntp_server);
	ESP_LOGI(TAG, "Using hostname %s", hostname);

	// the host name must be set for mDNS and LWIP
	ESP_LOGI(TAG, "setting hostname to %s", hostname);
	mdns_hostname_set(hostname);

	// Set timezone to Pacific Standard Time
	setenv("TZ", "PDT+7", 1);
	tzset();

	// start the scheduler
	esp_timer_start_periodic(schedule_timer, 1000000 * 60); // wake up once per minute
}
