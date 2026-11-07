#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <esp_system.h>

// WiFi, usuario/contraseña MQTT y certificado CA: en secrets.h (no se sube a GitHub).
// Plantilla con los mismos nombres: secrets.example.h
#include "secrets.h"

const char* DEVICE_ID = "esp32-huerta-01";

const char* TOPIC_TELEMETRY = "smartgarden/esp32/telemetry";
const char* TOPIC_STATUS    = "smartgarden/esp32/status";
const char* TOPIC_COMMANDS  = "smartgarden/esp32/commands";

// ---------------------------------------------------------------- conexiones físicas (no cambiar)
const int PIN_SENSOR     = 34;
const int PIN_RELE       = 26;
const int PIN_SENSOR_VCC = 25;
const int PIN_OLED_VCC   = 27;

// ---------------------------------------------------------------- calibración del sensor (no cambiar)
const int VALOR_SECO   = 4095;
const int VALOR_MOJADO = 1500;
const int CRUDO_MINIMO_VALIDO = 400;

const int HUMEDAD_CRITICA = 15;
const unsigned long MANUAL_MAX_SEG   = 120;
const unsigned long LECTURA_MS       = 500;

const bool RELE_ACTIVO_EN_BAJO = true;

// ---------------------------------------------------------------- envío en tiempo real (ajustables)
// Telemetría por eventos: la web ve los cambios en menos de 1 s sin gastar la cuota de EMQX (1 GB/mes).
// - Inmediata cuando cambia la bomba, sensorOk o la CONFIG recibida.
// - Por humedad cuando varía UMBRAL_CAMBIO_HUMEDAD puntos o más respecto al último valor enviado,
//   con al menos ENVIO_MIN_HUMEDAD_MS entre esos envíos.
// - Latido cada LATIDO_MS aunque no cambie nada (el backend marca "desconectado" a los 30 s sin datos).
// Tráfico esperado: 0,15-0,3 GB/mes. Si la consola de EMQX muestra más de 30 MB/día, subir el umbral a 3.
const int UMBRAL_CAMBIO_HUMEDAD           = 2;
const unsigned long ENVIO_MIN_HUMEDAD_MS  = 1000;
const unsigned long LATIDO_MS             = 10000;
// Promedio móvil de lecturas antes de comparar (cada lectura ya promedia 20 muestras). 1 = desactivado.
// Si el ruido del sensor dispara envíos de humedad, subir a 4 (ventana de 2 s).
const int VENTANA_SUAVIZADO               = 1;

// ---------------------------------------------------------------- red
const unsigned long RECONEXION_MS       = 10000;  // intento de MQTT cada 10 s
const unsigned long REINTENTO_NTP_MS    = 30000;  // nueva petición NTP si la hora no llega
const uint16_t MQTT_KEEPALIVE_S         = 30;
const uint16_t MQTT_SOCKET_TIMEOUT_S    = 10;     // los conectores corren en su propia tarea: no frenan el riego
const time_t HORA_VALIDA_DESDE          = 1704067200;  // 01/01/2024: antes de esto la hora no es real
const char* ZONA_HORARIA                = "<-05>5";    // Colombia, UTC-5 sin horario de verano

Adafruit_SSD1306 pantalla(128, 64, &Wire, -1);
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// Estado compartido entre la tarea de control (riego, sensor, pantalla) y la de red (loop).
SemaphoreHandle_t estadoMutex;

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
int historialHumedad[8];
int historialCuenta = 0;
int historialPos = 0;

// Eventos que la tarea de control pide publicar al instante.
volatile bool envioInmediato = true;
volatile bool wifiOk = false;
volatile bool nubeOk = false;

// Solo los usa la tarea de red.
unsigned long ultimaTelemetria = 0;
unsigned long ultimoEnvioHumedad = 0;
unsigned long ultimoIntentoMqtt = 0;
unsigned long ultimaPeticionNtp = 0;
int ultimaHumedadEnviada = -1;
bool ultimoSensorOkEnviado = false;
bool horaSincronizada = false;
bool ntpPedido = false;
bool wifiConectadoAntes = false;
bool mqttConectadoAntes = false;
unsigned long wifiCaidoDesde = 0;

void bloquear()    { xSemaphoreTake(estadoMutex, portMAX_DELAY); }
void desbloquear() { xSemaphoreGive(estadoMutex); }

void escribirRele(bool encender) {
  pinMode(PIN_RELE, OUTPUT);
  if (RELE_ACTIVO_EN_BAJO) digitalWrite(PIN_RELE, encender ? LOW : HIGH);
  else                     digitalWrite(PIN_RELE, encender ? HIGH : LOW);
}

// Llamar con el mutex tomado.
void bomba(bool encender) {
  if (encender == regando) return;
  regando = encender;
  escribirRele(encender);
  Serial.printf("Bomba %s\n", encender ? "ENCENDIDA" : "APAGADA");
  envioInmediato = true;
}

// Llamar con el mutex tomado.
void leerSensor() {
  long suma = 0;
  for (int i = 0; i < 20; i++) {
    suma += analogRead(PIN_SENSOR);
    delay(5);
  }
  crudo = suma / 20;
  bool okAntes = sensorOk;
  sensorOk = crudo >= CRUDO_MINIMO_VALIDO;
  if (sensorOk != okAntes) envioInmediato = true;
  int instantanea = constrain(map(crudo, VALOR_SECO, VALOR_MOJADO, 0, 100), 0, 100);

  const int ventana = constrain(VENTANA_SUAVIZADO, 1, 8);
  if (!sensorOk) {
    historialCuenta = 0;
    historialPos = 0;
    humedad = instantanea;
    return;
  }
  historialHumedad[historialPos] = instantanea;
  historialPos = (historialPos + 1) % ventana;
  if (historialCuenta < ventana) historialCuenta++;
  long total = 0;
  for (int i = 0; i < historialCuenta; i++) total += historialHumedad[i];
  humedad = (int)((total + historialCuenta / 2) / historialCuenta);
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

// Llamar con el mutex tomado.
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
  Serial.printf("CONFIG recibida: modo=%s min=%d max=%d lluvia=%d\n",
                modo.c_str(), umbralMin, umbralMax, pausaLluvia);

  if (cambioModo) {
    manualDuracionMs = 0;
    bomba(false);
  }
  envioInmediato = true;  // la web ve la configuración aplicada sin esperar al latido
}

// Llamar con el mutex tomado.
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
  bloquear();
  if (tipo == "CONFIG")    aplicarConfig(doc);
  else if (tipo == "PUMP") aplicarBomba(doc);
  desbloquear();
}

// Llamar con el mutex tomado.
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

// Llamar con el mutex tomado.
void mostrar() {
  pantalla.clearDisplay();
  pantalla.setTextColor(SSD1306_WHITE);
  pantalla.setTextSize(1);

  pantalla.setCursor(0, 0);
  pantalla.print(wifiOk ? "WiFi OK" : "Sin WiFi");
  pantalla.setCursor(70, 0);
  pantalla.print(nubeOk ? "Nube OK" : "Sin nube");

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

// ---------------------------------------------------------------- tarea de control (núcleo 1)
// Sensor, riego y pantalla cada 500 ms, pase lo que pase con la red: aunque el handshake TLS o un
// reintento MQTT tarde varios segundos, la bomba se sigue controlando con los umbrales guardados.
void tareaControl(void*) {
  TickType_t siguiente = xTaskGetTickCount();
  for (;;) {
    bloquear();
    leerSensor();
    controlarRiego();
    mostrar();
    desbloquear();
    vTaskDelayUntil(&siguiente, pdMS_TO_TICKS(LECTURA_MS));
  }
}

// ---------------------------------------------------------------- red (loop, núcleo 1 con menor prioridad)
void publicarTelemetria() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  bloquear();
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
  int humedadEnviada = humedad;
  bool sensorEnviado = sensorOk;
  envioInmediato = false;
  desbloquear();

  char buffer[384];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  if (!mqtt.publish(TOPIC_TELEMETRY, (const uint8_t*)buffer, n, false)) {
    // El socket TLS quedó muerto (p. ej. tras una caída de la red al arrancar la bomba): cerrar y reconectar ya,
    // sin esperar a que el keepalive lo detecte (hasta 45 s).
    Serial.println("No se pudo publicar: se cierra la conexión MQTT y se reconecta");
    mqtt.disconnect();
    wifiClient.stop();
    ultimoIntentoMqtt = 0;
    return;
  }
  ultimaTelemetria = millis();
  ultimoEnvioHumedad = ultimaTelemetria;
  ultimaHumedadEnviada = humedadEnviada;
  ultimoSensorOkEnviado = sensorEnviado;
}

// Hora real por NTP: sin ella falla la validación del certificado TLS.
bool horaLista() {
  if (horaSincronizada) return true;
  if (!ntpPedido || millis() - ultimaPeticionNtp >= REINTENTO_NTP_MS) {
    configTzTime(ZONA_HORARIA, "pool.ntp.org", "time.nist.gov");
    ntpPedido = true;
    ultimaPeticionNtp = millis();
    Serial.println("Sincronizando la hora por NTP...");
  }
  time_t ahora = time(nullptr);
  if (ahora < HORA_VALIDA_DESDE) return false;
  horaSincronizada = true;
  struct tm local;
  localtime_r(&ahora, &local);
  char texto[32];
  strftime(texto, sizeof(texto), "%Y-%m-%d %H:%M:%S", &local);
  Serial.printf("Hora sincronizada: %s (UTC-5)\n", texto);
  return true;
}

void mantenerConexion() {
  bool conectado = WiFi.status() == WL_CONNECTED;
  wifiOk = conectado;
  if (conectado != wifiConectadoAntes) {
    wifiConectadoAntes = conectado;
    if (conectado) Serial.printf("WiFi conectado. IP del ESP32: %s\n", WiFi.localIP().toString().c_str());
    else           Serial.println("WiFi desconectado, reintentando...");
  }
  if (!conectado) {
    nubeOk = false;
    mqttConectadoAntes = false;
    if (wifiCaidoDesde == 0) wifiCaidoDesde = millis();
    // Si la reconexión automática no lo logra en 20 s, forzarla.
    if (millis() - wifiCaidoDesde > 20000) {
      Serial.println("WiFi sin recuperarse, forzando reconexión");
      WiFi.reconnect();
      wifiCaidoDesde = millis();
    }
    return;
  }
  wifiCaidoDesde = 0;
  if (!horaLista()) return;
  if (mqtt.connected()) return;
  nubeOk = false;
  if (mqttConectadoAntes) {
    // Se acaba de perder: reintentar de inmediato en lugar de esperar RECONEXION_MS.
    mqttConectadoAntes = false;
    Serial.printf("Conexión MQTT perdida (estado %d), reconectando\n", mqtt.state());
    ultimoIntentoMqtt = 0;
  }
  if (ultimoIntentoMqtt != 0 && millis() - ultimoIntentoMqtt < RECONEXION_MS) return;
  ultimoIntentoMqtt = millis();

  Serial.printf("Conectando a EMQX (%s:%d, TLS)... ", SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  if (mqtt.connect(DEVICE_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    nubeOk = true;
    mqttConectadoAntes = true;
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_COMMANDS, 1);
    envioInmediato = true;
  } else {
    char error[100] = "";
    wifiClient.lastError(error, sizeof(error));
    Serial.printf("falló (estado %d)%s%s\n", mqtt.state(), error[0] ? " TLS: " : "", error);
  }
}

void decidirEnvio() {
  if (!mqtt.connected()) return;
  bloquear();
  bool inmediato = envioInmediato || sensorOk != ultimoSensorOkEnviado;
  int actual = humedad;
  desbloquear();

  unsigned long ahora = millis();
  bool cambioHumedad = ultimaHumedadEnviada < 0 || abs(actual - ultimaHumedadEnviada) >= UMBRAL_CAMBIO_HUMEDAD;
  if (inmediato) {
    publicarTelemetria();                                         // bomba, sensorOk o CONFIG
  } else if (cambioHumedad && ahora - ultimoEnvioHumedad >= ENVIO_MIN_HUMEDAD_MS) {
    publicarTelemetria();                                         // cambio real de humedad
  } else if (ahora - ultimaTelemetria >= LATIDO_MS) {
    publicarTelemetria();                                         // latido
  }
}

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "encendido";
    case ESP_RST_BROWNOUT: return "BAJADA DE VOLTAJE (brownout): revisa la alimentación de la bomba";
    case ESP_RST_PANIC:    return "error del programa (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_SW:       return "reinicio por software";
    default:               return "otro";
  }
}

void alEventoWifi(WiFiEvent_t evento, WiFiEventInfo_t info) {
  if (evento == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("WiFi desconectado (motivo %d)\n", info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Arranque: %s\n", motivoReinicio());
  estadoMutex = xSemaphoreCreateMutex();

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

  // El riego arranca ya, con la configuración de la NVS, aunque todavía no haya red.
  xTaskCreatePinnedToCore(tareaControl, "control", 6144, nullptr, 2, nullptr, 1);

  WiFi.mode(WIFI_STA);
  // Sin ahorro de energía del radio: evita retrasos y cortes de la conexión MQTT (tiempo real).
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(alEventoWifi);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

  // TLS con validación del certificado del broker (nunca setInsecure).
  wifiClient.setCACert(SECRET_MQTT_CA_CERT);
  wifiClient.setHandshakeTimeout(MQTT_SOCKET_TIMEOUT_S);
  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  mqtt.setCallback(alRecibirMensaje);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

void loop() {
  mantenerConexion();
  mqtt.loop();
  decidirEnvio();
  delay(10);
}
