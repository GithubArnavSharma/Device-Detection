#include "WiFi.h"
#include "esp_wifi.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiConfig.h>
#include <Adafruit_AHTX0.h>


// ── WiFi / Server config ──────────────────────────────────────────────────────
const char* WIFI_SSID     = ".......";
const char* WIFI_PASSWORD = ".......";
const char* N_VALUE_URL   = "http://192.168.1.96:5000/n_value";
const char* SERVER_URL   = "http://192.168.1.96:5000/classify";

#define MAX_TRACKED_DEVICES 30
const int MIN_EXPIRATION = 1;
const unsigned long DEVICE_EXPIRATION_MS = MIN_EXPIRATION * 60 * 1000;

unsigned long lastChannelSwitch = 0;
const unsigned long SWITCH_INTERVAL = 100;
const unsigned long N_CHECK_INTERVAL = 10000;
const unsigned long DEVICE_CHECK_INTERVAL = 1000;
const unsigned long CLOSEST_CHECK_INTERVAL = 5000;
uint8_t currentChannel = 1;

// ── Dynamic N (updated from server) ──────────────────────────────────────────
float current_n = 2.0;  // default until first fetch

LiquidCrystal_I2C lcd(0x27, 16, 2);

HardwareSerial SerialCamIn(1);

struct DeviceTracker {
    uint8_t mac[6];
    double current_distance;
    unsigned long last_seen;
    bool isActive;
};

DeviceTracker deviceList[MAX_TRACKED_DEVICES];


double calculate_distance(int8_t rssi) {
    const int8_t MEASURED_POWER = -59;
    if (rssi == 0) return -1.0;
    return pow(10.0, (double)(MEASURED_POWER - rssi) / (10.0 * current_n));
}

uint8_t new_channel(uint8_t current) {
    if (current == 1)  return 6;
    if (current == 6)  return 11;
    if (current == 11) return 1;
    return 1;
}

void sniff_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t* payload = pkt->payload;

    double current_distance = calculate_distance(rssi);
    if (current_distance < 0) return;

    uint8_t src_mac[6];
    memcpy(src_mac, &payload[10], 6);

    int deviceIndex = -1;
    int emptySlot = -1;

    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (deviceList[i].isActive && memcmp(deviceList[i].mac, src_mac, 6) == 0) {
            deviceIndex = i;
            break;
        }
        if (!deviceList[i].isActive && emptySlot == -1) {
            emptySlot = i;
        }
    }

    if (deviceIndex == -1) {
        if (emptySlot == -1) return;
        deviceIndex = emptySlot;
        deviceList[deviceIndex].isActive = true;
        memcpy(deviceList[deviceIndex].mac, src_mac, 6);
        deviceList[deviceIndex].current_distance = current_distance;
    }
    deviceList[deviceIndex].current_distance = current_distance;
    deviceList[deviceIndex].last_seen = millis();
}

void fetchN() {
    HTTPClient http;
    http.begin(N_VALUE_URL);
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        StaticJsonDocument<64> doc;
        deserializeJson(doc, body);
        current_n = doc["n_value"];
        Serial.printf("N updated: %.1f (%s)\n", current_n, doc["class"].as<const char*>());
    } else {
        Serial.println("Failed to fetch N, keeping current value");
    }
    http.end();
}


void switchChannel(void* param) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SWITCH_INTERVAL));
        currentChannel = new_channel(currentChannel);
    }
}

void checkN(void* param) {
    while (1) {
        if (WiFi.status() == WL_CONNECTED) fetchN();
        vTaskDelay(pdMS_TO_TICKS(N_CHECK_INTERVAL));
    }
}

void checkDevices(void* param) {
    while (1) {
        for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
            if (deviceList[i].isActive) {
                if (millis() - deviceList[i].last_seen > DEVICE_EXPIRATION_MS) {
                    deviceList[i].isActive = false;
                    continue;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CHECK_INTERVAL));
    }
}

void closestDeviceToLCD(void* param) {
    while (1) {
        double overall_closest = 999.0;
        int closest_index = -1;
        int active_count = 0;

        for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
            if (deviceList[i].isActive) {
                active_count++;
                if (deviceList[i].current_distance < overall_closest) {
                    overall_closest = deviceList[i].current_distance;
                    closest_index = i;
                }
            }
        }

        if (closest_index != -1) {
            uint8_t* m = deviceList[closest_index].mac;
            Serial.printf("N=%.1f | Tracking %d devices | Closest [%02X:%02X:%02X:%02X:%02X:%02X] at %.2fm\n",
                          current_n, active_count,
                          m[0], m[1], m[2], m[3], m[4], m[5],
                          overall_closest);
            //lcd.init();
            //lcd.backlight();
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Dist: " + String(overall_closest, 2) + " m");
            lcd.setCursor(0, 1);
            lcd.print("N: " + String(current_n));
        } else {
            Serial.println("No devices currently tracked in memory.");
        }

        vTaskDelay(pdMS_TO_TICKS(CLOSEST_CHECK_INTERVAL));
    }
}

void setup() {
    Serial.begin(115200);

    // ── Connect to WiFi ───────────────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        Serial.println(WiFi.status());
        attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFailed to connect — check SSID/password");
    } else {
        Serial.println("\nConnected: " + WiFi.localIP().toString());
        fetchN(); // initial fetch before sniffing starts
    }

    // ── Promiscuous mode ──────────────────────────────────────────────────────
    esp_wifi_set_promiscuous_rx_cb(&sniff_callback);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);

    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        deviceList[i].isActive = false;
    }

    lcd.init();
    lcd.backlight();
    lcd.clear();

    xTaskCreate(switchChannel, "SwitchChannel", 2048, NULL, 3, NULL);
    xTaskCreate(checkDevices, "CheckDevices", 4096, NULL, 2, NULL);
    xTaskCreate(checkN, "CheckN", 4096, NULL, 1, NULL);
    xTaskCreate(closestDeviceToLCD, "Closest", 4096, NULL, 1, NULL);
}


void loop() {
    
}
