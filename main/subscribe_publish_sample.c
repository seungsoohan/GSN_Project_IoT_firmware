#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include "lib.h"

static const char *TAG = "subpub";

/*
  gpio PART
*/
#define GPIO_OUTPUT_IO_0    2
#define GPIO_OUTPUT_IO_1    18
#define GPIO_OUTPUT_IO_2    19
#define GPIO_OUTPUT_IO_3    13
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1) | (1ULL<<GPIO_OUTPUT_IO_2) | (1ULL<<GPIO_OUTPUT_IO_3))
#define GPIO_INPUT_IO_0     27
#define GPIO_INPUT_IO_1     5
// #define GPIO_INPUT_IO_2     34
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1) )
#define ESP_INTR_FLAG_DEFAULT 0


#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   2          //Multisampling
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_6;     //GPIO34 if ADC1, GPIO14 if ADC2
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
static const adc_atten_t atten = ADC_ATTEN_DB_0;
static const adc_unit_t unit = ADC_UNIT_1;

static xQueueHandle gpio_evt_queue = NULL;


// #define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
// #define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD
#define EXAMPLE_WIFI_SSID "hans_home_2G"
#define EXAMPLE_WIFI_PASS "tq3131hans"
/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

const int CONNECTED_BIT = BIT0;


#if defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)

static const char * DEVICE_CERTIFICATE_PATH = CONFIG_EXAMPLE_CERTIFICATE_PATH;
static const char * DEVICE_PRIVATE_KEY_PATH = CONFIG_EXAMPLE_PRIVATE_KEY_PATH;
static const char * ROOT_CA_PATH = CONFIG_EXAMPLE_ROOT_CA_PATH;

#else
#error "Invalid method for loading certs"
#endif

AWS_IoT_Client client;
IoT_Error_t rc = FAILURE;
int cnt = 0;
int pir_count = -1;
int sound_count = -1;
/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
// char HostAddress[255] = AWS_IOT_MQTT_HOST;
char HostAddress[255] = "a2t0230y11azkm-ats.iot.ap-northeast-2.amazonaws.com";


uint32_t port = AWS_IOT_MQTT_PORT;


static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void mqtt_publish_task(void* arg) {
  while(1) {
    rc = aws_iot_mqtt_yield(&client, 100);
    if(NETWORK_ATTEMPTING_RECONNECT == rc) {
        continue;
    }

    const char *TOPIC1 = "iot/log";
    const int TOPIC_LEN1 = strlen(TOPIC1);
    char cPayload1[200];
    sprintf(cPayload1,
      "{\"office\": 1, \"sensor\":[{ \"type\": 1, \"name\": \"PIR\", \"count\": %d }, { \"type\": 2, \"name\":\"sound\", \"count\": %d } ] }", pir_count, sound_count
    );
    pir_count = 0;
    sound_count = 0;
    IoT_Publish_Message_Params paramsQOS01;
    paramsQOS01.qos = QOS0;
    paramsQOS01.payload = (void *) cPayload1;
    paramsQOS01.isRetained = 0;
    paramsQOS01.payloadLen = strlen(cPayload1);

    rc = aws_iot_mqtt_publish(&client, TOPIC1, TOPIC_LEN1, &paramsQOS01);

    printf("send to aws!!!!!!\n");

    vTaskDelay(30*1000 / portTICK_RATE_MS);
  }
}

static void sound_sensor_task(void* arg) {
  while(1) {
    uint32_t adc_reading = 0;
    //Multisampling
    for (int i = 0; i < NO_OF_SAMPLES; i++) {
        if (unit == ADC_UNIT_1) {
            adc_reading += adc1_get_raw((adc1_channel_t)channel);
        } else {
            int raw;
            adc2_get_raw((adc2_channel_t)channel, width, &raw);
            adc_reading += raw;
        }
    }
    adc_reading /= NO_OF_SAMPLES;
    //Convert adc_reading to voltage in mV
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);

    if(adc_reading < 4000) {
        printf("Raw: %d\tVoltage: %dmV\n", adc_reading, voltage);
        sound_count++;
        gpio_set_level(GPIO_OUTPUT_IO_3, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(GPIO_OUTPUT_IO_3, 0);
        vTaskDelay(pdMS_TO_TICKS(5700));
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
            if(io_num == 5 && gpio_get_level(io_num) == 1) {
                gpio_set_level(GPIO_OUTPUT_IO_0, 1);

                rc = aws_iot_mqtt_yield(&client, 100);
                if(NETWORK_ATTEMPTING_RECONNECT == rc) {
                    continue;
                }

                const char *TOPIC1 = "iot/topic";
                const int TOPIC_LEN1 = strlen(TOPIC1);
                char cPayload1[100];
                sprintf(cPayload1,
                  "{\"office\": 1, \"state\":0 }"
                );
                IoT_Publish_Message_Params paramsQOS01;
                paramsQOS01.qos = QOS0;
                paramsQOS01.payload = (void *) cPayload1;
                paramsQOS01.isRetained = 0;
                paramsQOS01.payloadLen = strlen(cPayload1);

                rc = aws_iot_mqtt_publish(&client, TOPIC1, TOPIC_LEN1, &paramsQOS01);

                vTaskDelay(300 / portTICK_RATE_MS);
                gpio_set_level(GPIO_OUTPUT_IO_0, 0);
            }
            else if(io_num == 27) {
               if(gpio_get_level(io_num) == 1) {
                 gpio_set_level(GPIO_OUTPUT_IO_0, 1);
                 pir_count++;
                 // rc = aws_iot_mqtt_yield(&client, 100);
                 // if(NETWORK_ATTEMPTING_RECONNECT == rc) {
                 //     continue;
                 // }
                 //
                 // const char *TOPIC1 = "iot/topic";
                 // const int TOPIC_LEN1 = strlen(TOPIC1);
                 // char cPayload1[100];
                 // sprintf(cPayload1,
                 //   "{\"office\": 1, \"state\":1 }"
                 // );
                 // IoT_Publish_Message_Params paramsQOS01;
                 // paramsQOS01.qos = QOS0;
                 // paramsQOS01.payload = (void *) cPayload1;
                 // paramsQOS01.isRetained = 0;
                 // paramsQOS01.payloadLen = strlen(cPayload1);
                 //
                 // rc = aws_iot_mqtt_publish(&client, TOPIC1, TOPIC_LEN1, &paramsQOS01);

               }
               else{
                 gpio_set_level(GPIO_OUTPUT_IO_0, 0);
               }
            }
        }
    }
}



static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(TAG, "Subscribe callback");
    ESP_LOGI(TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

void aws_iot_task(void *param) {
    char cPayload[100];

    int32_t i = 0;

    // IoT_Error_t rc = FAILURE;


    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;
    IoT_Publish_Message_Params paramsQOS1;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

#if defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)
    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)
    mqttInitParams.pRootCALocation = ROOT_CA_PATH;
    mqttInitParams.pDeviceCertLocation = DEVICE_CERTIFICATE_PATH;
    mqttInitParams.pDevicePrivateKeyLocation = DEVICE_PRIVATE_KEY_PATH;
#endif

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    // mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.tlsHandshakeTimeout_ms = 10000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;


    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    /* Client ID is set in the menuconfig of the example */
    connectParams.pClientID = CONFIG_AWS_EXAMPLE_CLIENT_ID;
    connectParams.clientIDLen = (uint16_t) strlen(CONFIG_AWS_EXAMPLE_CLIENT_ID);
    connectParams.isWillMsgPresent = false;


    ESP_LOGI(TAG, "Connecting to AWS...");
    do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            gpio_set_level(GPIO_OUTPUT_IO_2, 0);
            vTaskDelay(1000 / portTICK_RATE_MS);
            gpio_set_level(GPIO_OUTPUT_IO_2, 1);
        }
    } while(SUCCESS != rc);
    gpio_set_level(GPIO_OUTPUT_IO_2, 0);
    gpio_set_level(GPIO_OUTPUT_IO_1, 1);
    /*
     * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
     *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
     *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
     */
    rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    const char *TOPIC = "test_topic/esp32";
    const int TOPIC_LEN = strlen(TOPIC);

    ESP_LOGI(TAG, "Subscribing...");
    rc = aws_iot_mqtt_subscribe(&client, TOPIC, TOPIC_LEN, QOS0, iot_subscribe_callback_handler, NULL);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Error subscribing : %d ", rc);
        abort();
    }

    sprintf(cPayload, "%s : %d ", "hello from SDK", i);

    paramsQOS0.qos = QOS0;
    paramsQOS0.payload = (void *) cPayload;
    paramsQOS0.isRetained = 0;

    paramsQOS1.qos = QOS1;
    paramsQOS1.payload = (void *) cPayload;
    paramsQOS1.isRetained = 0;

    while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {

        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
            // If the client is attempting to reconnect we will skip the rest of the loop.
            continue;
        }

        ESP_LOGI(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(3000 / portTICK_RATE_MS);
        sprintf(cPayload, "%s : %d ", "hi hello from ESP32 (QOS0)", i++);
        paramsQOS0.payloadLen = strlen(cPayload);
        // rc = aws_iot_mqtt_publish(&client, TOPIC, TOPIC_LEN, &paramsQOS0);

        // sprintf(cPayload, "%s : %d ", "hello from ESP32 (QOS1)", i++);
        // paramsQOS1.payloadLen = strlen(cPayload);
        // rc = aws_iot_mqtt_publish(&client, TOPIC, TOPIC_LEN, &paramsQOS1);
        // if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
        //     ESP_LOGW(TAG, "QOS1 publish ack not received.");
        //     rc = SUCCESS;
        // }

    }


    ESP_LOGE(TAG, "An error occurred in the main loop.");
    abort();
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}


void app_main()
{
  // adc config
  adc1_config_width(width);
  adc1_config_channel_atten(channel, atten);
  adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));

  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  io_conf.intr_type = GPIO_INTR_POSEDGE;
  io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = 1;
  gpio_config(&io_conf);

  gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

  xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 2048, NULL, 9, NULL);

  xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);
  // sound sensor part
  xTaskCreate(sound_sensor_task, "sound_sensor_task", 2048, NULL, 11, NULL);


  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
  gpio_isr_handler_add(GPIO_INPUT_IO_1, gpio_isr_handler, (void*) GPIO_INPUT_IO_1);


  printf("Minimum free heap size: %d bytes\n", esp_get_minimum_free_heap_size());

    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    gpio_set_level(GPIO_OUTPUT_IO_2, 1);
    initialise_wifi();
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1);
}
