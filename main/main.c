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

#include <esp_http_server.h>

#ifndef PIN2STR
#define PIN2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5], (a)[6], (a)[7]
#define PINSTR "%c%c%c%c%c%c%c%c"
#endif

#define BLINK_SLOW 1000000
#define BLINK_FAST 250000

#define WATER_PIN GPIO_NUM_5

typedef struct program_state
{
	int led;			// the blue indicator led
	bool water;		// is the water circuit supposed to be on or not
} program_state;

static const char *TAG="APP";
static esp_wps_config_t wps_config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
static esp_timer_handle_t blink_timer;
static program_state state = 
{
	.led = 0
};

void toggle_led(void)
{
	state.led = !state.led;
	gpio_set_level(GPIO_NUM_2, state.led);
}

void led_off(void)
{
	state.led = 0;
	gpio_set_level(GPIO_NUM_2, state.led);
}

void blink_callback(void *arg)
{
	toggle_led();
}

void blink_start(uint64_t rate)
{
	ESP_LOGI(TAG, "blink_start");
	esp_timer_start_periodic(blink_timer, rate);
}

void blink_stop(void)
{
	ESP_LOGI(TAG, "blink_stop");
	esp_timer_stop(blink_timer);
}

esp_err_t blink_get_handler(httpd_req_t *req)
{
	char resp_str[12];
	sprintf(resp_str, "Led is %s", state.led ? "on" : "off"); 
	httpd_resp_send(req, resp_str, strlen(resp_str));

	toggle_led();

	return ESP_OK;
}

esp_err_t handler_water_off(httpd_req_t *req)
{
	char resp_str[14];

	gpio_set_level(WATER_PIN, 0);
	state.water = !!gpio_get_level(WATER_PIN);

	sprintf(resp_str, "Water is %s", state.water ? "on" : "off"); 
	httpd_resp_send(req, resp_str, strlen(resp_str));

	return ESP_OK;
}

esp_err_t handler_water_on(httpd_req_t *req)
{
	char resp_str[14];

	gpio_set_level(WATER_PIN, 1);
	state.water = !!gpio_get_level(WATER_PIN);

	sprintf(resp_str, "Water is %s", state.water ? "on" : "off"); 
	httpd_resp_send(req, resp_str, strlen(resp_str));

	return ESP_OK;
}

/* An HTTP GET handler */
esp_err_t hello_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[32];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "query1", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query1=%s", param);
            }
            if (httpd_query_key_value(buf, "query3", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query3=%s", param);
            }
            if (httpd_query_key_value(buf, "query2", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query2=%s", param);
            }
        }
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, strlen(resp_str));

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

httpd_uri_t uris[] = {
{
    .uri       = "/hello",
    .method    = HTTP_GET,
    .handler   = hello_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = "Hello World!"
},
{
    .uri       = "/blink",
    .method    = HTTP_GET,
    .handler   = blink_get_handler,
    .user_ctx  = "Blinking!"
},
{
    .uri       = "/water_on",
    .method    = HTTP_GET,
    .handler   = handler_water_on,
    .user_ctx  = ""
},
{
    .uri       = "/water_off",
    .method    = HTTP_GET,
    .handler   = handler_water_off,
    .user_ctx  = ""
}
};

httpd_handle_t start_webserver(void)
{
	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();

	// Start the httpd server
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
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

void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static httpd_handle_t server = NULL;

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
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
		ESP_LOGI(TAG, "setting hostname to bat");
		mdns_hostname_set("bat");

		ESP_LOGI(TAG, "starting mdnsd blink service");
		mdns_instance_name_set("blink");
		mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
	}
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
                system_event_sta_disconnected_t *event = (system_event_sta_disconnected_t *)event_data;

                ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
                if (event->reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
                    /*Switch to 802.11 bgn mode */
                    esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
                }
                ESP_ERROR_CHECK(esp_wifi_connect());
            }
            break;
        case WIFI_EVENT_STA_WPS_ER_SUCCESS:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_WPS_ER_SUCCESS");
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
            break;
        case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_WPS_ER_TIMEOUT");
            ESP_ERROR_CHECK(esp_wifi_wps_disable());
            ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
            ESP_ERROR_CHECK(esp_wifi_wps_start(0));
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
	const esp_timer_create_args_t blink_timer_args = {
		.callback = blink_callback,
		.arg = &blink_timer,
		.dispatch_method = ESP_TIMER_TASK,
		.name = ""
	};

	// set up hardware
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	tcpip_adapter_init();
	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
	gpio_set_direction(WATER_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(WATER_PIN, 0);

	// set up software
	esp_timer_create(&blink_timer_args, &blink_timer);
   ESP_ERROR_CHECK(nvs_flash_init());
   ESP_ERROR_CHECK(esp_netif_init());
   ESP_ERROR_CHECK(esp_event_loop_create_default());

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, &server));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
	ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
	ESP_ERROR_CHECK(esp_wifi_wps_start(0));
	ESP_ERROR_CHECK(mdns_init());
	blink_start(BLINK_SLOW);
}
