#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// --- Cấu hình phần cứng ---
#define DHTPIN        4
#define DHTTYPE       DHT22
#define PUMP_PIN      25
#define FAN_PIN       26
#define MIST_PIN      27

#define PWM_FREQ      5000
#define PWM_RES       8 

// --- Cấu hình Network & MQTT ---
const char* ssid         = "Wokwi-GUEST";
const char* password     = "";
const char* mqtt_server  = "broker.hivemq.com";
const int   mqtt_port    = 1883;

// Topics
const char* topic_sensor = "farm/telemetry/data";
const char* topic_pump   = "farm/control/pump";
const char* topic_fan    = "farm/control/fan";
const char* topic_mist   = "farm/control/mist";

const char* status_pump  = "farm/status/pump";
const char* status_fan   = "farm/status/fan";
const char* status_mist  = "farm/status/mist";

const char* topic_system_mode = "farm/system/mode"; // Topic tổng: AUTO hoặc MANUAL

WiFiClient espClient;
PubSubClient client(espClient);

DHT dht(DHTPIN, DHTTYPE);

struct SensorData {
  float temperature;
  float humidity;
};

struct MqttMessage {
  char topic[32];
  char payload[16];
};

QueueHandle_t dataQueue;
QueueHandle_t dataAlertQueue;
QueueHandle_t mqttQueue;

// Biến trạng thái chế độ tổng (true = MANUAL, false = AUTO)
bool isSystemManual = false;

String pumpStatus   = "OFF";
String fanStatus    = "OFF";
String mistStatus   = "OFF";

unsigned long mistStartTime = 0;
unsigned long mistDuration = 0;
bool isMistRunning = false;
bool mistLocked = false; 

void TaskSendData(void *pvParameters);
void TaskReadData(void *pvParameters);
void TaskAlert(void *pvParameters);
void mqttCallback(char* topic, byte* payload, unsigned int length);

void publishStatus(const char* tpc, const char* val) {
  MqttMessage msg;
  strlcpy(msg.topic, tpc, sizeof(msg.topic));
  strlcpy(msg.payload, val, sizeof(msg.payload));
  xQueueSend(mqttQueue, &msg, 0);
}

void setup() {
  Serial.begin(115200);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  ledcAttach(MIST_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(MIST_PIN, 0);

  dht.begin();

  dataQueue      = xQueueCreate(10, sizeof(SensorData));
  dataAlertQueue = xQueueCreate(1, sizeof(float));
  mqttQueue      = xQueueCreate(20, sizeof(MqttMessage));

  if (dataQueue == NULL || dataAlertQueue == NULL || mqttQueue == NULL) {
    Serial.println("Loi tao Queue!");
    while (1);
  }
  
  Serial.println("Tao Queue thanh cong!");

  xTaskCreatePinnedToCore(TaskSendData, "TaskSendData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskReadData, "TaskReadData", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskAlert,    "TaskAlert",    4096, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// -------------------------------------------------------------------
// TASK 1: TRUYỀN DATA & MQTT (CORE 0)
// -------------------------------------------------------------------
void TaskSendData(void *pvParameters) {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  
  Serial.println("MQTT da ket noi!");

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);

  SensorData receivedData;
  MqttMessage mqttMsg;

  for (;;) {
    if (!client.connected()) {
      String clientId = "ESP32FarmClient-" + String(random(0xffff), HEX);
      if (client.connect(clientId.c_str())) {
        client.subscribe(topic_pump);
        client.subscribe(topic_fan);
        client.subscribe(topic_mist);
        client.subscribe(topic_system_mode);
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    client.loop();

    if (xQueueReceive(dataQueue, &receivedData, pdMS_TO_TICKS(50)) == pdTRUE) {
      char jsonBuffer[128];
      snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"temp\":%.2f, \"humi\":%.2f}", 
               receivedData.temperature, receivedData.humidity);
      client.publish(topic_sensor, jsonBuffer);
    }

    while (xQueueReceive(mqttQueue, &mqttMsg, 0) == pdTRUE) {
      client.publish(mqttMsg.topic, mqttMsg.payload);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Xử lý lệnh từ Web
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  
  // Xử lý đổi chế độ Tổng
  if (String(topic) == topic_system_mode) {
    isSystemManual = (message == "MANUAL");
    Serial.printf("Chuyen che do tong: %s\n", message.c_str());
  }
  
  // Xử lý điều khiển thủ công (Chỉ thực thi khi đang ở chế độ MANUAL)
  else if (isSystemManual) {
    if (String(topic) == topic_pump) {
      bool pOn = (message == "ON" || message == "true");
      digitalWrite(PUMP_PIN, pOn ? HIGH : LOW);
      pumpStatus = pOn ? "ON" : "OFF";
      client.publish(status_pump, pumpStatus.c_str());
    } 
    else if (String(topic) == topic_fan) {
      bool fOn = (message == "ON" || message == "true");
      digitalWrite(FAN_PIN, fOn ? HIGH : LOW);
      fanStatus = fOn ? "ON" : "OFF";
      client.publish(status_fan, fanStatus.c_str());
    }
    else if (String(topic) == topic_mist) {
      int mistVal = message.toInt();
      if (mistVal == 0) {
        ledcWrite(MIST_PIN, 0);
        isMistRunning = false;
        mistDuration = 0;
        mistStatus = "OFF";
        client.publish(status_mist, "0");
      } else {
        uint8_t pwmDuty = (mistVal * 255) / 100;
        ledcWrite(MIST_PIN, pwmDuty);
        isMistRunning = true;
        mistDuration = 999999; 
        mistStatus = String(mistVal);
        client.publish(status_mist, message.c_str());
      }
    }
  }
}

// -------------------------------------------------------------------
// TASK 2: ĐỌC DATA & ĐIỀU KHIỂN TỰ ĐỘNG (CORE 1)
// -------------------------------------------------------------------
void TaskReadData(void *pvParameters) {
  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      Serial.printf("Nhiet do: %.1f C\n", t);
      Serial.printf("Do am: %.1f %%\n", h);

      SensorData data = { t, h };
      xQueueSend(dataQueue, &data, portMAX_DELAY);
      xQueueOverwrite(dataAlertQueue, &h);

      if (h >= 40.0) {
        mistLocked = false;
      }

      // Chỉ chạy TỰ ĐỘNG khi hệ thống ở chế độ AUTO (!isSystemManual)
      if (!isSystemManual) {
        // Logic Bơm
        bool pumpOn = (h < 40.0);
        digitalWrite(PUMP_PIN, pumpOn ? HIGH : LOW);
        String newPumpStatus = pumpOn ? "ON" : "OFF";
        if (pumpStatus != newPumpStatus) {
          pumpStatus = newPumpStatus;
          publishStatus(status_pump, pumpStatus.c_str());
        }

        // Logic Quạt
        bool fanOn = (h > 60.0 || t > 30.0);
        digitalWrite(FAN_PIN, fanOn ? HIGH : LOW);
        String newFanStatus = fanOn ? "ON" : "OFF";
        if (fanStatus != newFanStatus) {
          fanStatus = newFanStatus;
          publishStatus(status_fan, fanStatus.c_str());
        }
      }

      Serial.println("Trang thai:");
      Serial.printf("- Che do: %s\n", isSystemManual ? "MANUAL" : "AUTO");
      Serial.printf("- May bom: %s\n", pumpStatus.c_str());
      Serial.printf("- Quat: %s\n", fanStatus.c_str());
      Serial.printf("- Phun suong: %s\n", mistStatus.c_str());
      Serial.println("-----------------------------------");

    } else {
      Serial.println("[ERROR] Loi doc cam bien DHT22!");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// -------------------------------------------------------------------
// TASK 3: PHUN SƯƠNG TỰ ĐỘNG
// -------------------------------------------------------------------
void TaskAlert(void *pvParameters) {
  float humiAlert = 0.0;

  for (;;) {
    if (isMistRunning && mistDuration < 999999) {
      if (millis() - mistStartTime >= mistDuration) {
        ledcWrite(MIST_PIN, 0);
        isMistRunning = false;
        mistLocked = true; 
        mistStatus = "OFF";
        publishStatus(status_mist, "0");
      }
    }

    if (xQueueReceive(dataAlertQueue, &humiAlert, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Chỉ tự động phun sương khi KHÔNG ở MANUAL
      if (!isSystemManual && !isMistRunning && !mistLocked) {
        uint8_t pwmDuty = 0;
        String levelStr = "0";

        if (humiAlert < 30.0) {
          pwmDuty = 255;          
          mistDuration = 30000;   
          levelStr = "100";
        } else if (humiAlert >= 30.0 && humiAlert < 35.0) {
          pwmDuty = (uint8_t)(255 * 0.50); 
          mistDuration = 20000;   
          levelStr = "50";
        } else if (humiAlert >= 35.0 && humiAlert < 40.0) {
          pwmDuty = (uint8_t)(255 * 0.25); 
          mistDuration = 10000;   
          levelStr = "25";
        }

        if (pwmDuty > 0) {
          ledcWrite(MIST_PIN, pwmDuty);
          mistStartTime = millis();
          isMistRunning = true;
          mistStatus = levelStr;
          publishStatus(status_mist, levelStr.c_str());
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}