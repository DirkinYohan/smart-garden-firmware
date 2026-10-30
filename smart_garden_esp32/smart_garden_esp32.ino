/*
  Smart Garden - Firmware ESP32 (riego local + WiFi + MQTT)

  Conexiones (las mismas que ya funcionan):
  - Sensor MH:  AO -> GPIO34, VCC -> GPIO25, GND -> GND, DO sin conectar
  - Relé:       IN -> GPIO26, VCC -> 3V3, GND -> GND
  - OLED:       VCC -> GPIO27, GND -> GND, SDA -> GPIO21, SCL -> GPIO22

  Librerías: Adafruit SSD1306, Adafruit GFX, PubSubClient, ArduinoJson (v7)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================== CONFIGURA ESTO ==================
const char* WIFI_SSID = "Tp link Jen";     // red de 2.4 GHz
const char* WIFI_PASS = "98394Mor*";
const char* MQTT_HOST = "192.168.0.224";          // IP de tu PC con Docker
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";                      // vacío si Mosquitto es anónimo
const char* MQTT_PASS = "";
const char* DEVICE_ID = "esp32-huerta-01";
// ====================================================

const char* TOPIC_TELEMETRY = "smartgarden/esp32/telemetry";
const char* TOPIC_STATUS    = "smartgarden/esp32/status";
const char* TOPIC_COMMANDS  = "smartgarden/esp32/commands";

const int PIN_SENSOR     = 34;
const int PIN_RELE       = 26;
const int PIN_SENSOR_VCC = 25;
const int PIN_OLED_VCC   = 27;

// Calibración del sensor (ajústala)
const int VALOR_SECO   = 4095;
const int VALOR_MOJADO = 1500;
const int CRUDO_MINIMO_VALIDO = 400;

const int HUMEDAD_CRITICA = 15;
const unsigned long MANUAL_MAX_SEG   = 120;
const unsigned long LECTURA_MS       = 500;   // lee el sensor 2 veces por segundo
const unsigned long TELEMETRIA_MS    = 1000;  // latido cada segundo: el panel va en tiempo real
const unsigned long RECONEXION_MS    = 10000;

const bool RELE_ACTIVO_EN_BAJO = true;

Adafruit_SSD1306 pantalla(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

String modo = "AUTO";
int umbralMin = 30;
int umbralMax = 70;
bool pausaLluvia = false;

bool regando = false;
int crudo = 0;
int humedad = 0;
bool sensorOk = false;
unsigned long manualInicio = 0;
unsigned long manualDuracionMs = 0;
unsigned long ultimaLectura = 0;
unsigned long ultimaTelemetria = 0;
unsigned long ultimoIntentoMqtt = 0;
int ultimaHumedadEnviada = -1;
bool ultimoSensorOkEnviado = false;

void escribirRele(bool encender) {
  pinMode(PIN_RELE, OUTPUT);
  if (RELE_ACTIVO_EN_BAJO) digitalWrite(PIN_RELE, encender ? LOW : HIGH);
  else                     digitalWrite(PIN_RELE, encender ? HIGH : LOW);
}

void publicarTelemetria();

void bomba(bool encender) {
  if (encender == regando) return;
  regando = encender;
  escribirRele(encender);
  Serial.printf("Bomba %s\n", encender ? "ENCENDIDA" : "APAGADA");
  publicarTelemetria();
}

void leerSensor() {
  long suma = 0;
  for (int i = 0; i < 20; i++) {
    suma += analogRead(PIN_SENSOR);
    delay(5);
  }
  crudo = suma / 20;
  sensorOk = crudo >= CRUDO_MINIMO_VALIDO;
  humedad = constrain(map(crudo, VALOR_SECO, VALOR_MOJADO, 0, 100), 0, 100);
}

void cargarConfig() {
  prefs.begin("garden", true);
  modo        = prefs.getString("modo", "AUTO");
  umbralMin   = prefs.getInt("min", 30);
  umbralMax   = prefs.getInt("max", 70);
  pausaLluvia = prefs.getBool("lluvia", false);
  prefs.end();
}

void guardarConfig() {
  prefs.begin("garden", false);
  prefs.putString("modo", modo);
  prefs.putInt("min", umbralMin);
  prefs.putInt("max", umbralMax);
  prefs.putBool("lluvia", pausaLluvia);
  prefs.end();
}

void aplicarConfig(JsonDocument& doc) {
  String nuevoModo = doc["mode"] | modo;
  int nuevoMin     = doc["minMoisture"] | umbralMin;
  int nuevoMax     = doc["maxMoisture"] | umbralMax;
  bool nuevaPausa  = doc["rainPause"] | pausaLluvia;

  bool modoValido = nuevoModo == "AUTO" || nuevoModo == "MANUAL" || nuevoModo == "AI";
  bool rangoValido = nuevoMin >= 0 && nuevoMax <= 100 && (nuevoMax - nuevoMin) >= 10;
  if (!modoValido || !rangoValido) {
    Serial.println("CONFIG inválida, se ignora");
    return;
  }

  bool cambioModo = nuevoModo != modo;
  modo = nuevoModo;
  umbralMin = nuevoMin;
  umbralMax = nuevoMax;
  pausaLluvia = nuevaPausa;
  guardarConfig();
  Serial.printf("CONFIG: modo=%s min=%d max=%d lluvia=%d\n",
                modo.c_str(), umbralMin, umbralMax, pausaLluvia);

  if (cambioModo) {
    manualDuracionMs = 0;
    bomba(false);
  }
}

void aplicarBomba(JsonDocument& doc) {
  if (modo != "MANUAL") {
    Serial.println("PUMP ignorado: no está en modo MANUAL");
    return;
  }
  String accion = doc["action"] | "";
  if (accion == "ON") {
    unsigned long seg = doc["durationSec"] | 30;
    seg = constrain(seg, 1UL, MANUAL_MAX_SEG);
    manualInicio = millis();
    manualDuracionMs = seg * 1000UL;
    bomba(true);
  } else if (accion == "OFF") {
    manualDuracionMs = 0;
    bomba(false);
  }
}

void alRecibirMensaje(char* topic, byte* payload, unsigned int largo) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, largo)) {
    Serial.println("Mensaje MQTT no es JSON válido");
    return;
  }
  String tipo = doc["type"] | "";
  if (tipo == "CONFIG")    aplicarConfig(doc);
  else if (tipo == "PUMP") aplicarBomba(doc);
}

bool wifiConectadoAntes = false;

void mantenerConexion() {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk != wifiConectadoAntes) {
    wifiConectadoAntes = wifiOk;
    if (wifiOk) Serial.printf("WiFi conectado. IP del ESP32: %s\n", WiFi.localIP().toString().c_str());
    else        Serial.println("WiFi desconectado, reintentando...");
  }
  if (!wifiOk) return;
  if (mqtt.connected()) return;
  if (millis() - ultimoIntentoMqtt < RECONEXION_MS) return;
  ultimoIntentoMqtt = millis();

  Serial.print("Conectando a MQTT... ");
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
  if (mqtt.connect(DEVICE_ID, user, pass, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_COMMANDS, 1);
    publicarTelemetria();
  } else {
    Serial.printf("falló (estado %d)\n", mqtt.state());
  }
}

void publicarTelemetria() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["deviceId"]    = DEVICE_ID;
  doc["raw"]         = crudo;
  doc["moisture"]    = humedad;
  doc["pumpOn"]      = regando;
  doc["sensorOk"]    = sensorOk;
  doc["mode"]        = modo;
  doc["minMoisture"] = umbralMin;
  doc["maxMoisture"] = umbralMax;
  doc["rainPause"]   = pausaLluvia;
  doc["uptimeMs"]    = millis();

  char buffer[384];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(TOPIC_TELEMETRY, (const uint8_t*)buffer, n, false);
  ultimaTelemetria = millis();
  ultimaHumedadEnviada = humedad;
  ultimoSensorOkEnviado = sensorOk;
}

void controlarRiego() {
  if (!sensorOk) {
    bomba(false);
    return;
  }
  if (modo == "MANUAL") {
    if (regando && millis() - manualInicio >= manualDuracionMs) bomba(false);
    return;
  }
  bool puedeIniciar = !pausaLluvia || humedad < HUMEDAD_CRITICA;
  if (!regando && humedad < umbralMin && puedeIniciar) bomba(true);
  else if (regando && humedad > umbralMax)             bomba(false);
}

void mostrar() {
  pantalla.clearDisplay();
  pantalla.setTextColor(SSD1306_WHITE);
  pantalla.setTextSize(1);

  pantalla.setCursor(0, 0);
  pantalla.print(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "Sin WiFi");
  pantalla.setCursor(70, 0);
  pantalla.print(mqtt.connected() ? "Nube OK" : "Sin nube");

  if (sensorOk) {
    pantalla.setTextSize(3);
    pantalla.setCursor(0, 14);
    pantalla.print(humedad);
    pantalla.print("%");
  } else {
    pantalla.setCursor(0, 20);
    pantalla.print("REVISAR SENSOR");
  }

  pantalla.setTextSize(1);
  pantalla.setCursor(0, 40);
  pantalla.print("Bomba: ");
  pantalla.print(regando ? "REGANDO" : "apagada");

  pantalla.setCursor(0, 52);
  pantalla.printf("%s %d-%d%%", modo.c_str(), umbralMin, umbralMax);
  if (pausaLluvia) pantalla.print(" lluvia");

  pantalla.display();
}

void setup() {
  Serial.begin(115200);

  escribirRele(false);

  pinMode(PIN_SENSOR_VCC, OUTPUT);
  digitalWrite(PIN_SENSOR_VCC, HIGH);
  pinMode(PIN_OLED_VCC, OUTPUT);
  digitalWrite(PIN_OLED_VCC, HIGH);
  delay(100);

  analogSetAttenuation(ADC_11db);

  if (!pantalla.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("No se encontró la pantalla OLED");
  }

  cargarConfig();
  Serial.printf("Config guardada: modo=%s min=%d max=%d\n",
                modo.c_str(), umbralMin, umbralMax);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(alRecibirMensaje);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);
}

void loop() {
  mantenerConexion();
  mqtt.loop();

  if (millis() - ultimaLectura >= LECTURA_MS) {
    ultimaLectura = millis();
    leerSensor();
    controlarRiego();
    mostrar();

    // Tiempo real: si la humedad cambió, se envía de inmediato
    if (humedad != ultimaHumedadEnviada || sensorOk != ultimoSensorOkEnviado) {
      publicarTelemetria();
    }
  }

  if (millis() - ultimaTelemetria >= TELEMETRIA_MS) {
    publicarTelemetria();
    if (!mqtt.connected()) ultimaTelemetria = millis();
  }
}
