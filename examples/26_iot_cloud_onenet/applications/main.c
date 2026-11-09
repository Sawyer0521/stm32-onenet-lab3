/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-09-01              first implementation
 * 2026-06-10     GuZheng     OneNET OneJSON demo
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include "wifi_config.h"
#include <stdlib.h>
#include <onenet.h>
#include <string.h>
#include <ap3216c.h>
#include <easyflash.h>
#include <drv_gpio.h>
#include "paho_mqtt.h"
#include "cloud_config.h"

#define FW_VERSION "v1.1.0"

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#define I2C_BUS_NAME "i2c1"
#define LED_PIN PIN_LED_R
#define MOTOR_PIN_A PIN_MOTOR_A
#define MOTOR_PIN_B PIN_MOTOR_B
#define UPLOAD_INTERVAL_MS (5 * 1000)
#define CONTROL_PWM_PERIOD_STEPS 20
#define CONTROL_PWM_STEP_MS 1

/*
 * OneNET product information:
 * Product ID: configured in cloud_config.h
 * Device name: configured in cloud_config.h
 * Properties: humi(float), lux(float), temp(float), Led(int), Motor(int)
 */

#define MQTT_URI "tcp://mqtts.heclouds.com:1883"
#define MQTT_CLIENT_ID ONENET_DEVICE_NAME
#define MQTT_USERNAME ONENET_PRODUCT_ID

#define MQTT_PUBTOPIC_PROPERTY_POST "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post"
#define MQTT_PUBTOPIC_PROPERTY_SET_REPLY "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set_reply"
#define MQTT_SUBTOPIC_PROPERTY_SET "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set"
#define MQTT_SUBTOPIC_PROPERTY_POST_REPLY "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post/reply"

#define MQTT_WORK_BUFFER_SIZE 1024
#define MQTT_JSON_BUFFER_SIZE 512
#define MQTT_RX_TEXT_BUFFER_SIZE 256

typedef struct
{
    int led;
    int motor;
    int humi_x10;
    int lux_x10;
    int temp_x10;
} onenet_data_t;

ap3216c_device_t dev;
static MQTTClient mqtt_client;
static volatile int led_brightness = 50;
static volatile int motor_speed = 0;
static rt_uint16_t mqtt_pub_packet_id = 0;
static rt_bool_t control_pwm_started = RT_FALSE;

static void mqtt_start(void);
static void onenet_upload_cycle(void);
static void control_pwm_start(void);

static rt_uint16_t mqtt_next_packet_id(void)
{
    mqtt_pub_packet_id++;
    if (mqtt_pub_packet_id == 0)
    {
        mqtt_pub_packet_id = 1;
    }

    return mqtt_pub_packet_id;
}

static int topic_equals(MQTTString *topic, const char *expect)
{
    int expect_len;

    if (topic == RT_NULL || expect == RT_NULL)
    {
        return 0;
    }

    expect_len = strlen(expect);
    if (topic->lenstring.len != expect_len)
    {
        return 0;
    }

    return strncmp(topic->lenstring.data, expect, expect_len) == 0;
}

static void mqtt_payload_get_id(const char *payload, char *out, int out_len)
{
    const char *id;
    const char *colon;
    const char *begin;
    int i = 0;

    if (out == RT_NULL || out_len <= 0)
    {
        return;
    }

    rt_snprintf(out, out_len, "0");
    if (payload == RT_NULL)
    {
        return;
    }

    id = strstr(payload, "\"id\"");
    if (id == RT_NULL)
    {
        return;
    }

    colon = strchr(id, ':');
    if (colon == RT_NULL)
    {
        return;
    }

    begin = colon + 1;
    while (*begin == ' ' || *begin == '\"')
    {
        begin++;
    }

    while (begin[i] != '\0' && begin[i] != '\"' && begin[i] != ',' &&
           begin[i] != '}' && i < out_len - 1)
    {
        out[i] = begin[i];
        i++;
    }
    out[i] = '\0';
}

static int mqtt_payload_get_bool(const char *payload, const char *key, int *value)
{
    const char *key_pos;
    const char *value_pos;
    const char *colon;

    if (payload == RT_NULL || key == RT_NULL || value == RT_NULL)
    {
        return 0;
    }

    key_pos = strstr(payload, key);
    if (key_pos == RT_NULL)
    {
        return 0;
    }

    value_pos = strstr(key_pos, "\"value\"");
    if (value_pos != RT_NULL)
    {
        colon = strchr(value_pos, ':');
    }
    else
    {
        colon = strchr(key_pos, ':');
    }

    if (colon == RT_NULL)
    {
        return 0;
    }

    colon++;
    while (*colon == ' ' || *colon == '\"')
    {
        colon++;
    }

    if (strncmp(colon, "true", 4) == 0 || *colon == '1')
    {
        *value = 1;
    }
    else
    {
        *value = 0;
    }

    return 1;
}

static int mqtt_payload_get_int(const char *payload, const char *key, int *value)
{
    const char *key_pos;
    const char *value_pos;
    const char *colon;

    if (payload == RT_NULL || key == RT_NULL || value == RT_NULL)
    {
        return 0;
    }

    key_pos = strstr(payload, key);
    if (key_pos == RT_NULL)
    {
        return 0;
    }

    value_pos = strstr(key_pos, "\"value\"");
    if (value_pos != RT_NULL)
    {
        colon = strchr(value_pos, ':');
    }
    else
    {
        colon = strchr(key_pos, ':');
    }

    if (colon == RT_NULL)
    {
        return 0;
    }

    colon++;
    while (*colon == ' ' || *colon == '\"')
    {
        colon++;
    }

    *value = atoi(colon);
    return 1;
}

static int clamp_percent(int value)
{
    if (value < 0)
    {
        return 0;
    }

    if (value > 100)
    {
        return 100;
    }

    return value;
}

static void control_apply_led(int value)
{
    led_brightness = clamp_percent(value);

    if (led_brightness <= 0)
    {
        rt_pin_write(LED_PIN, PIN_HIGH);
    }
    else if (led_brightness >= 100)
    {
        rt_pin_write(LED_PIN, PIN_LOW);
    }

    rt_kprintf("[CTRL] Led brightness=%d%%\r\n", led_brightness);
}

static void control_apply_motor(int value)
{
    motor_speed = clamp_percent(value);

    if (motor_speed <= 0)
    {
        rt_pin_write(MOTOR_PIN_A, PIN_LOW);
        rt_pin_write(MOTOR_PIN_B, PIN_LOW);
    }
    else if (motor_speed >= 100)
    {
        rt_pin_write(MOTOR_PIN_A, PIN_HIGH);
        rt_pin_write(MOTOR_PIN_B, PIN_LOW);
    }

    rt_kprintf("[CTRL] Motor speed=%d%%\r\n", motor_speed);
}

static void control_pwm_entry(void *parameter)
{
    int step = 0;
    int led_on_steps;
    int motor_on_steps;

    (void)parameter;

    while (1)
    {
        led_on_steps = (led_brightness * CONTROL_PWM_PERIOD_STEPS + 99) / 100;
        motor_on_steps = (motor_speed * CONTROL_PWM_PERIOD_STEPS + 99) / 100;

        if (led_brightness > 0 && led_brightness < 100)
        {
            rt_pin_write(LED_PIN, step < led_on_steps ? PIN_LOW : PIN_HIGH);
        }

        if (motor_speed > 0 && motor_speed < 100)
        {
            rt_pin_write(MOTOR_PIN_A, step < motor_on_steps ? PIN_HIGH : PIN_LOW);
            rt_pin_write(MOTOR_PIN_B, PIN_LOW);
        }

        step++;
        if (step >= CONTROL_PWM_PERIOD_STEPS)
        {
            step = 0;
        }

        rt_thread_mdelay(CONTROL_PWM_STEP_MS);
    }
}

static void control_pwm_start(void)
{
    rt_thread_t tid;

    if (control_pwm_started)
    {
        return;
    }

    tid = rt_thread_create("ctrl_pwm",
                           control_pwm_entry,
                           RT_NULL,
                           1024,
                           RT_THREAD_PRIORITY_MAX / 2,
                           5);
    if (tid)
    {
        rt_thread_startup(tid);
        control_pwm_started = RT_TRUE;
    }
}

static int light_sensor_read_x10(int *lux_x10)
{
    float lux = 0.0f;

    if (lux_x10 == RT_NULL)
    {
        return -RT_ERROR;
    }

    if (dev == RT_NULL)
    {
        *lux_x10 = 0;
        return -RT_ERROR;
    }

    lux = ap3216c_read_ambient_light(dev);
    if (lux < 0.0f)
    {
        *lux_x10 = 0;
        return -RT_ERROR;
    }

    if (lux > 100000.0f)
    {
        lux = 100000.0f;
    }

    *lux_x10 = (int)(lux * 10.0f + 0.5f);
    return RT_EOK;
}

static void onenet_collect_data(onenet_data_t *data)
{
    rt_uint32_t seconds;
    int lux_status;

    if (data == RT_NULL)
    {
        return;
    }

    seconds = (rt_uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);
    lux_status = light_sensor_read_x10(&data->lux_x10);

    data->led = led_brightness;
    data->motor = motor_speed;
    data->temp_x10 = 240 + (int)((seconds / 17) % 80); /* 24.0 - 31.9 C */
    data->humi_x10 = 450 + (int)((seconds / 23) % 250); /* 45.0 - 69.9 % */

    if (lux_status != RT_EOK)
    {
        data->lux_x10 = 0;
    }
}

static int mqtt_publish_set_reply(const char *msg_id, int code, const char *msg)
{
    char json_buf[128];
    MQTTMessage message;
    int len;
    int mqtt_ret;

    len = rt_snprintf(json_buf, sizeof(json_buf),
        "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
        msg_id == RT_NULL ? "0" : msg_id,
        code,
        msg == RT_NULL ? "" : msg);
    if (len <= 0 || len >= (int)sizeof(json_buf))
    {
        return -RT_ERROR;
    }

    message.qos = QOS1;
    message.retained = 0;
    message.dup = 0;
    message.id = mqtt_next_packet_id();
    message.payload = json_buf;
    message.payloadlen = len;

    rt_kprintf("[OneNET] MQTT set_reply topic: %s\r\n", MQTT_PUBTOPIC_PROPERTY_SET_REPLY);
    rt_kprintf("[OneNET] MQTT set_reply packet_id=%d, payload=%s\r\n", message.id, json_buf);

    mqtt_ret = MQTTPublish(&mqtt_client, MQTT_PUBTOPIC_PROPERTY_SET_REPLY, &message);
    rt_kprintf("[OneNET] property set reply mqtt_ret=%d, code=%d\r\n", mqtt_ret, code);

    return mqtt_ret;
}

static void mqtt_handle_property_set(const char *payload)
{
    char msg_id[32];
    int value = 0;
    int handled = 0;

    mqtt_payload_get_id(payload, msg_id, sizeof(msg_id));

    if (mqtt_payload_get_int(payload, "Led", &value) ||
        mqtt_payload_get_int(payload, "LED", &value) ||
        mqtt_payload_get_int(payload, "led", &value))
    {
        control_apply_led(value);
        handled = 1;
    }

    if (mqtt_payload_get_int(payload, "Motor", &value) ||
        mqtt_payload_get_int(payload, "MOTOR", &value) ||
        mqtt_payload_get_int(payload, "motor", &value))
    {
        control_apply_motor(value);
        handled = 1;
    }

    if (handled)
    {
        LOG_I("cloud set Led=%d, Motor=%d", led_brightness, motor_speed);
        mqtt_publish_set_reply(msg_id, 200, "success");
    }
    else
    {
        mqtt_publish_set_reply(msg_id, 400, "unsupported property");
    }
}

static int mqtt_publish_property(const onenet_data_t *data)
{
    static rt_uint32_t msg_id = 0;
    char json_buf[MQTT_JSON_BUFFER_SIZE];
    MQTTMessage message;
    int len;
    int mqtt_ret;

    if (data == RT_NULL)
    {
        return -RT_ERROR;
    }

    len = rt_snprintf(json_buf, sizeof(json_buf),
        "{\"id\":\"%u\",\"version\":\"1.0\",\"params\":{"
            "\"Led\":{\"value\":%d},"
            "\"Motor\":{\"value\":%d},"
            "\"humi\":{\"value\":%d.%d},"
            "\"lux\":{\"value\":%d.%d},"
            "\"temp\":{\"value\":%d.%d}"
        "}}",
        (unsigned int)++msg_id,
        data->led,
        data->motor,
        data->humi_x10 / 10, data->humi_x10 % 10,
        data->lux_x10 / 10, data->lux_x10 % 10,
        data->temp_x10 / 10, data->temp_x10 % 10);
    if (len <= 0 || len >= (int)sizeof(json_buf))
    {
        rt_kprintf("[OneNET] property json buffer is too small, len=%d\r\n", len);
        return -RT_ERROR;
    }

    message.qos = QOS1;
    message.retained = 0;
    message.dup = 0;
    message.id = mqtt_next_packet_id();
    message.payload = json_buf;
    message.payloadlen = len;

    rt_kprintf("[OneNET] MQTT publish broker URI: %s\r\n", MQTT_URI);
    rt_kprintf("[OneNET] MQTT publish topic: %s\r\n", MQTT_PUBTOPIC_PROPERTY_POST);
    rt_kprintf("[OneNET] MQTT publish URL: %s/%s\r\n", MQTT_URI, MQTT_PUBTOPIC_PROPERTY_POST);
    rt_kprintf("[OneNET] MQTT publish qos=%d, packet_id=%d, payload_len=%d\r\n",
               message.qos,
               message.id,
               len);
    rt_kprintf("[OneNET] MQTT publish payload: %s\r\n", json_buf);

    mqtt_ret = MQTTPublish(&mqtt_client, MQTT_PUBTOPIC_PROPERTY_POST, &message);
    rt_kprintf("[OneNET] MQTT publish local status mqtt_ret=%d, %s\r\n",
               mqtt_ret,
               mqtt_ret == 0 ? "sent to MQTT worker, wait cloud reply code" : "send failed or MQTT offline");

    return mqtt_ret;
}

static void onenet_upload_entry(void *parameter)
{
    onenet_data_t data;
    int mqtt_ret;

    while (1)
    {
        if (!mqtt_client.isconnected)
        {
            rt_kprintf("[OneNET] MQTT is offline, wait before next property post\r\n");
            rt_thread_mdelay(UPLOAD_INTERVAL_MS);
            continue;
        }

        onenet_collect_data(&data);

        mqtt_ret = mqtt_publish_property(&data);
        if (mqtt_ret < 0)
        {
            rt_kprintf("[OneNET] upload has an error, mqtt_ret=%d, retry next cycle\r\n", mqtt_ret);
        }
        else
        {
            rt_kprintf("[OneNET] publish mqtt_ret=%d, Led=%d, Motor=%d, humi=%d.%d, lux=%d.%d, temp=%d.%d\r\n",
                mqtt_ret,
                data.led,
                data.motor,
                data.humi_x10 / 10, data.humi_x10 % 10,
                data.lux_x10 / 10, data.lux_x10 % 10,
                data.temp_x10 / 10, data.temp_x10 % 10);
        }

        rt_thread_mdelay(UPLOAD_INTERVAL_MS);
    }
}

static void onenet_upload_cycle(void)
{
    static rt_bool_t is_started = RT_FALSE;
    rt_thread_t tid;

    if (is_started)
    {
        return;
    }

    tid = rt_thread_create("onenet_send",
                           onenet_upload_entry,
                           RT_NULL,
                           2 * 1024,
                           RT_THREAD_PRIORITY_MAX / 3 - 1,
                           5);
    if (tid)
    {
        rt_thread_startup(tid);
        is_started = RT_TRUE;
    }
}
MSH_CMD_EXPORT(onenet_upload_cycle, upload data to onenet);

static void mqtt_sub_callback(MQTTClient *c, MessageData *msg_data)
{
    char payload[MQTT_RX_TEXT_BUFFER_SIZE];
    char msg_id[32];
    int payload_len;
    int copy_len;
    int code;

    if (msg_data == RT_NULL || msg_data->message == RT_NULL ||
        msg_data->topicName == RT_NULL)
    {
        return;
    }

    payload_len = (int)msg_data->message->payloadlen;
    copy_len = payload_len;
    if (copy_len > (int)sizeof(payload) - 1)
    {
        copy_len = sizeof(payload) - 1;
    }

    memcpy(payload, msg_data->message->payload, copy_len);
    payload[copy_len] = '\0';

    rt_kprintf("[OneNET] MQTT RX topic: %.*s\r\n",
          msg_data->topicName->lenstring.len,
          msg_data->topicName->lenstring.data);
    rt_kprintf("[OneNET] MQTT RX payload: %.*s\r\n", copy_len, payload);

    if (topic_equals(msg_data->topicName, MQTT_SUBTOPIC_PROPERTY_POST_REPLY))
    {
        mqtt_payload_get_id(payload, msg_id, sizeof(msg_id));
        if (mqtt_payload_get_int(payload, "code", &code))
        {
            rt_kprintf("[OneNET] property post reply id=%s, cloud_status_code=%d\r\n", msg_id, code);
        }
        else
        {
            rt_kprintf("[OneNET] property post reply id=%s, cloud_status_code=unknown\r\n", msg_id);
        }
    }

    if (topic_equals(msg_data->topicName, MQTT_SUBTOPIC_PROPERTY_SET))
    {
        mqtt_handle_property_set(payload);
    }
}

static void mqtt_default_callback(MQTTClient *c, MessageData *msg_data)
{
    if (msg_data == RT_NULL || msg_data->message == RT_NULL ||
        msg_data->topicName == RT_NULL)
    {
        return;
    }

    LOG_D("default topic: %.*s payload: %.*s",
          msg_data->topicName->lenstring.len,
          msg_data->topicName->lenstring.data,
          (int)msg_data->message->payloadlen,
          (char *)msg_data->message->payload);
}

static void mqtt_connect_callback(MQTTClient *c)
{
    LOG_I("Start to connect mqtt server: %s", MQTT_URI);
}

static void mqtt_online_callback(MQTTClient *c)
{
    rt_kprintf("[OneNET] OneNET thing-model MQTT online\r\n");
    onenet_upload_cycle();
}

static void mqtt_offline_callback(MQTTClient *c)
{
    rt_kprintf("[OneNET] OneNET thing-model MQTT offline\r\n");
}

static void mqtt_start(void)
{
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;
    static char cid[20] = {0};
    static int is_started = 0;

    if (is_started)
    {
        return;
    }

    mqtt_client.isconnected = 0;
    mqtt_client.uri = MQTT_URI;

    rt_snprintf(cid, sizeof(cid), "%s", MQTT_CLIENT_ID);

    memcpy(&mqtt_client.condata, &condata, sizeof(condata));
    mqtt_client.condata.clientID.cstring = cid;
    mqtt_client.condata.keepAliveInterval = 60;
    mqtt_client.condata.cleansession = 1;
    mqtt_client.condata.username.cstring = MQTT_USERNAME;
    mqtt_client.condata.password.cstring = MQTT_PASSWORD;
    mqtt_client.condata.willFlag = 0;

    mqtt_client.buf_size = mqtt_client.readbuf_size = MQTT_WORK_BUFFER_SIZE;
    mqtt_client.buf = malloc(mqtt_client.buf_size);
    mqtt_client.readbuf = malloc(mqtt_client.readbuf_size);
    if (!(mqtt_client.buf && mqtt_client.readbuf))
    {
        LOG_E("no memory for MQTT client buffer!");
        if (mqtt_client.buf)
        {
            free(mqtt_client.buf);
            mqtt_client.buf = RT_NULL;
        }
        if (mqtt_client.readbuf)
        {
            free(mqtt_client.readbuf);
            mqtt_client.readbuf = RT_NULL;
        }
        return;
    }

    mqtt_client.connect_callback = mqtt_connect_callback;
    mqtt_client.online_callback = mqtt_online_callback;
    mqtt_client.offline_callback = mqtt_offline_callback;

    mqtt_client.messageHandlers[0].topicFilter = MQTT_SUBTOPIC_PROPERTY_POST_REPLY;
    mqtt_client.messageHandlers[0].callback = mqtt_sub_callback;
    mqtt_client.messageHandlers[0].qos = QOS1;

    mqtt_client.messageHandlers[1].topicFilter = MQTT_SUBTOPIC_PROPERTY_SET;
    mqtt_client.messageHandlers[1].callback = mqtt_sub_callback;
    mqtt_client.messageHandlers[1].qos = QOS1;
    mqtt_client.defaultMessageHandler = mqtt_default_callback;

    rt_kprintf("[OneNET] MQTT broker URI: %s\r\n", MQTT_URI);
    rt_kprintf("[OneNET] MQTT client id: %s\r\n", MQTT_CLIENT_ID);
    rt_kprintf("[OneNET] MQTT username/product id: %s\r\n", MQTT_USERNAME);
    rt_kprintf("[OneNET] MQTT subscribe topic[0]: %s\r\n", MQTT_SUBTOPIC_PROPERTY_POST_REPLY);
    rt_kprintf("[OneNET] MQTT subscribe topic[1]: %s\r\n", MQTT_SUBTOPIC_PROPERTY_SET);
    paho_mqtt_start(&mqtt_client);
    is_started = 1;
}

static void wlan_ready_handler(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)event;
    (void)buff;
    (void)parameter;

    mqtt_start();
}

int main(void)
{
    rt_kprintf("[APP] Firmware version: %s\n", FW_VERSION);
    rt_pin_mode(LED_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(MOTOR_PIN_A, PIN_MODE_OUTPUT);
    rt_pin_mode(MOTOR_PIN_B, PIN_MODE_OUTPUT);
    rt_pin_write(LED_PIN, PIN_HIGH);
    rt_pin_write(MOTOR_PIN_A, PIN_LOW);
    rt_pin_write(MOTOR_PIN_B, PIN_LOW);
    control_pwm_start();

    dev = ap3216c_init(I2C_BUS_NAME);
    if (dev == RT_NULL)
    {
        LOG_E("AP3216C init failed, lux will be reported as 0");
    }
    else
    {
        LOG_I("AP3216C ready on %s", I2C_BUS_NAME);
    }

    rt_wlan_register_event_handler(RT_WLAN_EVT_READY,
                                   wlan_ready_handler,
                                   RT_NULL);

    wlan_autoconnect_init();
    rt_wlan_config_autoreconnect(RT_TRUE);

    LOG_I("Connecting WiFi: %s", WLAN_SSID);
    rt_wlan_connect(WLAN_SSID, WLAN_PASSWORD);

    return 0;
}

static void onenet_data_status(void)
{
    onenet_data_t data;

    onenet_collect_data(&data);
    rt_kprintf("Led=%d, Motor=%d, humi=%d.%d, lux=%d.%d, temp=%d.%d\n",
               data.led,
               data.motor,
               data.humi_x10 / 10, data.humi_x10 % 10,
               data.lux_x10 / 10, data.lux_x10 % 10,
               data.temp_x10 / 10, data.temp_x10 % 10);
}
MSH_CMD_EXPORT(onenet_data_status, show OneNET property values);

static int onenet_led_set(int argc, char **argv)
{
    if (argc != 2)
    {
        rt_kprintf("usage: onenet_led_set [0-100]\n");
        return -RT_ERROR;
    }

    control_apply_led(atoi(argv[1]));
    return RT_EOK;
}
MSH_CMD_EXPORT(onenet_led_set, set Led brightness percent);

static int onenet_motor_set(int argc, char **argv)
{
    if (argc != 2)
    {
        rt_kprintf("usage: onenet_motor_set [0-100]\n");
        return -RT_ERROR;
    }

    control_apply_motor(atoi(argv[1]));
    return RT_EOK;
}
MSH_CMD_EXPORT(onenet_motor_set, set Motor speed percent);

rt_err_t onenet_port_save_device_info(char *dev_id, char *api_key)
{
    EfErrCode err = EF_NO_ERR;

    err = ef_set_and_save_env("dev_id", dev_id);
    if (err != EF_NO_ERR)
    {
        LOG_E("save device info(dev_id : %s) failed!", dev_id);
        return -RT_ERROR;
    }

    err = ef_set_and_save_env("api_key", api_key);
    if (err != EF_NO_ERR)
    {
        LOG_E("save device info(api_key : %s) failed!", api_key);
        return -RT_ERROR;
    }

    err = ef_set_and_save_env("already_register", "1");
    if (err != EF_NO_ERR)
    {
        LOG_E("save already_register failed!");
        return -RT_ERROR;
    }

    return RT_EOK;
}

rt_err_t onenet_port_get_register_info(char *dev_name, char *auth_info)
{
    rt_uint32_t cpuid[2] = {0};
    EfErrCode err = EF_NO_ERR;

    cpuid[0] = *(volatile rt_uint32_t *)(0x1FFF7590);
    cpuid[1] = *(volatile rt_uint32_t *)(0x1FFF7590 + 4);

    rt_snprintf(dev_name, ONENET_INFO_AUTH_LEN, "%d%d", cpuid[0], cpuid[1]);
    rt_snprintf(auth_info, ONENET_INFO_AUTH_LEN, "%d%d", cpuid[0], cpuid[1]);

    err = ef_set_and_save_env("auth_info", auth_info);
    if (err != EF_NO_ERR)
    {
        LOG_E("save auth_info failed!");
        return -RT_ERROR;
    }

    return RT_EOK;
}

rt_err_t onenet_port_get_device_info(char *dev_id, char *api_key, char *auth_info)
{
    char *info = RT_NULL;

    info = ef_get_env("dev_id");
    if (info == RT_NULL)
    {
        LOG_E("read dev_id failed!");
        return -RT_ERROR;
    }
    else
    {
        rt_snprintf(dev_id, ONENET_INFO_AUTH_LEN, "%s", info);
    }

    info = ef_get_env("api_key");
    if (info == RT_NULL)
    {
        LOG_E("read api_key failed!");
        return -RT_ERROR;
    }
    else
    {
        rt_snprintf(api_key, ONENET_INFO_AUTH_LEN, "%s", info);
    }

    info = ef_get_env("auth_info");
    if (info == RT_NULL)
    {
        LOG_E("read auth_info failed!");
        return -RT_ERROR;
    }
    else
    {
        rt_snprintf(auth_info, ONENET_INFO_AUTH_LEN, "%s", info);
    }

    return RT_EOK;
}

rt_bool_t onenet_port_is_registed(void)
{
    char *already_register = RT_NULL;

    already_register = ef_get_env("already_register");
    if (already_register == RT_NULL)
    {
        return RT_FALSE;
    }

    return already_register[0] == '1' ? RT_TRUE : RT_FALSE;
}

static void auto_task(void *arg)
{
    rt_thread_mdelay(8000);
    if (mqtt_client.isconnected)
    {
        onenet_upload_cycle();
    }
    else
    {
        LOG_I("MQTT is not online yet, upload will start after online callback");
    }
}

int auto_start(void)
{
    rt_thread_t tid = rt_thread_create("auto", auto_task, RT_NULL, 4096, 15, 5);
    if (tid)
    {
        rt_thread_startup(tid);
    }
    return 0;
}
INIT_APP_EXPORT(auto_start);
