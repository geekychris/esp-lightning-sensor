#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "mqtt_client.h"

// MQTT Configuration
#define MQTT_BROKER_URI "mqtt://your_broker_ip"
#define MQTT_USERNAME "your_username"
#define MQTT_PASSWORD "your_password"
#define MQTT_TOPIC "lightning/detection"

// Lightning Detector Configuration
#define I2C_MASTER_SCL_IO 22        // SCL pin
#define I2C_MASTER_SDA_IO 21        // SDA pin
#define I2C_MASTER_NUM I2C_NUM_0    // I2C port number
#define I2C_MASTER_FREQ_HZ 100000   // I2C frequency
#define WCMCU3935_ADDR 0x03         // I2C address of WCMCU-3935

// Lightning Detector Registers
#define REG_LIGHTNING_STATUS 0x02
#define REG_INT_MASK 0x03
#define REG_ENERGY_LIGHT_LSB 0x04
#define REG_ENERGY_LIGHT_MSB 0x05

static esp_mqtt_client_handle_t mqtt_client;
static const char* TAG = "lightning_detector";

// I2C initialization
static esp_err_t i2c_master_init() {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// Write to I2C device
static esp_err_t i2c_write_byte(uint8_t reg_addr, uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (WCMCU3935_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Read from I2C device
static esp_err_t i2c_read_byte(uint8_t reg_addr, uint8_t* data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (WCMCU3935_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (WCMCU3935_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
        default:
            break;
    }
}

// Initialize MQTT
static void mqtt_init() {
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Initialize Lightning Detector
static void lightning_detector_init() {
    // Configure interrupt mask
    i2c_write_byte(REG_INT_MASK, 0x0F);  // Enable all interrupts
}

// Check for lightning events
static void check_lightning() {
    uint8_t status;
    if (i2c_read_byte(REG_LIGHTNING_STATUS, &status) == ESP_OK) {
        if (status & 0x08) {  // Lightning detected
            uint8_t energy_lsb, energy_msb;
            i2c_read_byte(REG_ENERGY_LIGHT_LSB, &energy_lsb);
            i2c_read_byte(REG_ENERGY_LIGHT_MSB, &energy_msb);
            
            uint16_t energy = (energy_msb << 8) | energy_lsb;
            
            char message[64];
            snprintf(message, sizeof(message), "{\"event\":\"lightning\",\"energy\":%d}", energy);
            
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, message, 0, 1, 0);
            ESP_LOGI(TAG, "Lightning detected! Energy: %d", energy);
            
            // Clear the interrupt
            i2c_write_byte(REG_LIGHTNING_STATUS, 0x08);
        }
    }
}

extern "C" void app_main() {
    // Initialize I2C
    ESP_ERROR_CHECK(i2c_master_init());
    
    // Initialize Lightning Detector
    lightning_detector_init();
    
    // Initialize MQTT
    mqtt_init();
    
    while (1) {
        check_lightning();
        vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
    }
}

