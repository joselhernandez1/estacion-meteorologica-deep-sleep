/*
 * ESTACIÓN METEOROLÓGICA CON AHORRO ENERGÉTICO (DEEP SLEEP)
 * ESP32 + DHT22 + LDR + LCD I2C 16x2 + LED indicador
 * HTTP POST a ThingSpeak. Memoria RTC y RTC Timer Wakeup.
 * LED: 2 parpadeos = envío exitoso, 5 parpadeos = error.
 *
 * SIMULADOR_WOKWI 1 -> duerme en tramos de 1 s (adaptación para Wokwi)
 * SIMULADOR_WOKWI 0 -> un solo sueño largo (diseño real para hardware)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

// ===================== CONFIGURACIÓN =====================
#define SIMULADOR_WOKWI 1  // 1 = tramos de 1 s (Wokwi) | 0 = sueño largo (hardware real)
#define MODO_PRUEBA     1  // 1 = periodo de 20 s (demo) | 0 = periodo de 5 min (final)

const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const char* TS_URL     = "https://api.thingspeak.com/update";
const char* TS_API_KEY = "PEGA_AQUI_TU_WRITE_API_KEY";

#define PIN_DHT      15
#define PIN_LDR      34
#define PIN_LED      2
#define PIN_I2C_SDA  21
#define PIN_I2C_SCL  22
#define LCD_DIR      0x27

const bool LDR_INVERTIDO = false;

#if MODO_PRUEBA
  const uint64_t TIEMPO_SUENO_S = 20;
#else
  const uint64_t TIEMPO_SUENO_S = 300;       // 5 minutos
#endif

const uint32_t TIEMPO_PANTALLA_MS = 10000;   // datos visibles 10 s
const uint32_t WIFI_TIMEOUT_MS    = 10000;   // máximo por intento (limita el consumo si no hay señal)
const uint8_t  WIFI_REINTENTOS    = 2;
const uint8_t  HTTP_REINTENTOS    = 3;

// ===================== MEMORIA RTC =====================
RTC_DATA_ATTR int   contadorDespertares = 0;
RTC_DATA_ATTR float ultimaTemperatura   = 0;
RTC_DATA_ATTR float ultimaHumedad       = 0;
RTC_DATA_ATTR int   ultimoNivelSolar    = 0;

RTC_DATA_ATTR int     enviosOK         = 0;
RTC_DATA_ATTR int     enviosFallidos   = 0;
RTC_DATA_ATTR int     ultimoCodigoHTTP = 0;
RTC_DATA_ATTR bool    wifiGuardado     = false;   // caché de canal y BSSID
RTC_DATA_ATTR int32_t wifiCanal        = 0;
RTC_DATA_ATTR uint8_t wifiBssid[6]     = {0};
RTC_DATA_ATTR int     tramosPendientes = 0;       // solo modo simulador

// ===================== OBJETOS =====================
LiquidCrystal_I2C lcd(LCD_DIR, 16, 2);
DHT dht(PIN_DHT, DHT22);

// ===================== SENSORES Y LED =====================
bool leerDHT(float &t, float &h) {
  for (int i = 0; i < 3; i++) {
    float tt = dht.readTemperature();
    float hh = dht.readHumidity();
    if (!isnan(tt) && !isnan(hh)) { t = tt; h = hh; return true; }
    delay(2100);   // el DHT22 necesita >2 s entre lecturas
  }
  return false;
}

int leerNivelSolar() {
  analogSetPinAttenuation(PIN_LDR, ADC_11db);
  uint32_t suma = 0;
  for (int i = 0; i < 16; i++) { suma += analogRead(PIN_LDR); delay(5); }
  int pct = map(suma / 16, 0, 4095, 0, 100);
  if (LDR_INVERTIDO) pct = 100 - pct;
  return constrain(pct, 0, 100);
}

void parpadearLED(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(PIN_LED, HIGH); delay(150);
    digitalWrite(PIN_LED, LOW);  delay(150);
  }
}

// ===================== WIFI =====================
bool conectarWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  for (uint8_t intento = 1; intento <= WIFI_REINTENTOS; intento++) {
    Serial.printf("[WiFi] Intento %u/%u\n", intento, WIFI_REINTENTOS);

    if (wifiGuardado && intento == 1) {
      WiFi.begin(WIFI_SSID, WIFI_PASS, wifiCanal, wifiBssid);   // conexión rápida
    } else {
      if (intento > 1) {            // solo se reinicia la conexión en los reintentos
        WiFi.disconnect();
        delay(100);
      }
      WiFi.begin(WIFI_SSID, WIFI_PASS, 6);                       // canal 6 en Wokwi
    }

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifiCanal = WiFi.channel();
      memcpy(wifiBssid, WiFi.BSSID(), 6);
      wifiGuardado = true;
      Serial.printf("[WiFi] Conectado. IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    wifiGuardado = false;   // la caché pudo quedar obsoleta
  }
  Serial.println("[WiFi] No se pudo conectar");
  return false;
}

// ===================== HTTP POST =====================
// Devuelve el código HTTP, o un número negativo si hubo error.
int enviarUnaVez(float t, float h, int solar) {
  WiFiClientSecure cliente;
  cliente.setInsecure();   // sin validar certificado (aceptable en prototipo)
  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(cliente, TS_URL)) return -100;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String cuerpo = String("api_key=") + TS_API_KEY +
                  "&field1=" + String(t, 1) +
                  "&field2=" + String(h, 1) +
                  "&field3=" + String(solar) +
                  "&field4=" + String(contadorDespertares);

  int codigo = http.POST(cuerpo);
  if (codigo > 0) {
    String resp = http.getString();            // entry_id; "0" = rechazado
    Serial.printf("[HTTP] Respuesta ThingSpeak: %s\n", resp.c_str());
    if (codigo == 200 && resp.toInt() == 0) codigo = -200;
  } else {
    Serial.printf("[HTTP] Error de conexión: %s\n", HTTPClient::errorToString(codigo).c_str());
  }
  http.end();
  return codigo;
}

void describirError(int c) {
  if (c == -200)                 Serial.println("[HTTP] ThingSpeak rechazó el dato (clave inválida o menos de 15 s)");
  else if (c == 400)             Serial.println("[HTTP] 400: formato de datos incorrecto");
  else if (c == 401 || c == 403) Serial.println("[HTTP] 401/403: API key inválida");
  else if (c == 404)             Serial.println("[HTTP] 404: URL incorrecta");
  else if (c == 429)             Serial.println("[HTTP] 429: demasiadas peticiones");
  else if (c >= 500)             Serial.println("[HTTP] 5xx: error del servidor");
  else if (c < 0)                Serial.println("[HTTP] Error de red o timeout");
}

bool enviarConReintentos(float t, float h, int solar) {
  for (uint8_t i = 1; i <= HTTP_REINTENTOS; i++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[HTTP] WiFi caído, reconectando...");
      if (!conectarWiFi()) return false;
    }
    int codigo = enviarUnaVez(t, h, solar);
    ultimoCodigoHTTP = codigo;
    Serial.printf("[HTTP] Intento %u/%u -> código %d\n", i, HTTP_REINTENTOS, codigo);
    if (codigo == 200) return true;
    describirError(codigo);
    if (codigo == 400 || codigo == 401 || codigo == 403 || codigo == 404) break;  // reintentar no sirve
    delay(codigo == -200 ? 16000 : 2000);   // ThingSpeak exige 15 s entre envíos
  }
  return false;
}

// ===================== LCD =====================
void mostrarDatos(float t, float h, int solar, bool lecturaOK) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:"); lcd.print(t, 1); lcd.print((char)223); lcd.print("C ");
  lcd.print("H:"); lcd.print(h, 0); lcd.print("%");
  lcd.setCursor(0, 1);
  lcd.print("Sol:"); lcd.print(solar); lcd.print("% ");
  lcd.print(lecturaOK ? "#" : "!"); lcd.print(contadorDespertares);   // "!" = dato anterior
}

void mostrarEstado(bool wifiOK, bool envioOK) {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (!wifiOK)      lcd.print("WiFi: ERROR");
  else if (envioOK) lcd.print("Nube: OK");
  else              lcd.print("Nube: ERROR");
  lcd.setCursor(0, 1);
  lcd.print("OK:"); lcd.print(enviosOK);
  lcd.print(" ERR:"); lcd.print(enviosFallidos);
}

// ===================== DEEP SLEEP =====================
void irADormir(uint64_t inicioUs) {
  WiFi.disconnect(true, false);   // apaga la radio
  WiFi.mode(WIFI_OFF);
  lcd.noBacklight();
  lcd.noDisplay();
  digitalWrite(PIN_LED, LOW);

  // Descuenta el tiempo activo de ESTE ciclo para mantener el periodo total
  uint64_t activoUs  = esp_timer_get_time() - inicioUs;
  uint64_t periodoUs = TIEMPO_SUENO_S * 1000000ULL;
  uint64_t suenoUs   = (activoUs < periodoUs) ? (periodoUs - activoUs) : 1000000ULL;

  Serial.printf("[SLEEP] Activo %.1f s. Durmiendo %.1f s. Despertares: %d\n",
                activoUs / 1e6, suenoUs / 1e6, contadorDespertares);

#if SIMULADOR_WOKWI
  int segundos = (int)((suenoUs + 500000ULL) / 1000000ULL);   // redondea a segundos
  if (segundos < 1) segundos = 1;
  tramosPendientes = segundos - 1;                            // tramos extra tras este
  Serial.printf("[SIM] Modo simulador: %d tramos de 1 s\n", segundos);
  suenoUs = 1000000ULL;
#endif

  Serial.flush();
  esp_sleep_enable_timer_wakeup(suenoUs);   // RTC Timer Wakeup
  esp_deep_sleep_start();                   // no retorna
}

// ===================== DIAGNÓSTICO =====================
void imprimirMotivoArranque() {
  esp_reset_reason_t motivo = esp_reset_reason();
  Serial.printf("[BOOT] Motivo de reinicio: %d ", (int)motivo);
  switch (motivo) {
    case ESP_RST_POWERON:   Serial.println("(encendido o Play)"); break;
    case ESP_RST_SW:        Serial.println("(reinicio por software)"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("(despertar de Deep Sleep)"); break;
    default:                Serial.println("(REINICIO INESPERADO: watchdog u otro; la memoria RTC se pierde)"); break;
  }
}

// ===================== SETUP =====================
void setup() {
  uint64_t inicioUs = esp_timer_get_time();   // marca de inicio de este ciclo

  Serial.begin(115200);
  delay(100);

#if SIMULADOR_WOKWI
  // Tramos intermedios: no hacen nada más que volver a dormir
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && tramosPendientes > 0) {
    tramosPendientes--;
    Serial.printf("[zZz] tramo, faltan %d s\n", tramosPendientes + 1);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(1000000ULL);
    esp_deep_sleep_start();
  }
#endif

  delay(100);
  contadorDespertares++;
  esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();
  Serial.printf("\n=== Despertar #%d | causa: %s ===\n", contadorDespertares,
                causa == ESP_SLEEP_WAKEUP_TIMER ? "TIMER RTC" : "ARRANQUE/RESET");
  imprimirMotivoArranque();
  Serial.printf("[RTC] Última T=%.1f H=%.1f Sol=%d%% | OK=%d ERR=%d | último HTTP=%d\n",
                ultimaTemperatura, ultimaHumedad, ultimoNivelSolar,
                enviosOK, enviosFallidos, ultimoCodigoHTTP);

  // 1) Inicializaciones
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_LDR, INPUT);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Estacion Meteo");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  dht.begin();

  uint32_t inicioPantalla = millis();

  // 2) Lecturas (si el DHT falla se usa el último valor guardado en RTC)
  float t = ultimaTemperatura, h = ultimaHumedad;
  bool lecturaOK = leerDHT(t, h);
  int solar = leerNivelSolar();
  Serial.printf("[SENSORES] T=%.1f C | H=%.1f %% | Sol=%d %% | DHT %s\n",
                t, h, solar, lecturaOK ? "OK" : "FALLO, uso último valor");

  // 3) Guardar en RTC
  ultimaTemperatura = t;
  ultimaHumedad     = h;
  ultimoNivelSolar  = solar;

  // 4) Mostrar datos y mientras tanto enviar
  mostrarDatos(t, h, solar, lecturaOK);
  bool wifiOK  = conectarWiFi();
  bool envioOK = wifiOK && enviarConReintentos(t, h, solar);

  if (envioOK) enviosOK++; else enviosFallidos++;
  mostrarEstado(wifiOK, envioOK);
  parpadearLED(envioOK ? 2 : 5);

  // 5) Completar 10 s de pantalla
  uint32_t transcurrido = millis() - inicioPantalla;
  if (transcurrido < TIEMPO_PANTALLA_MS) delay(TIEMPO_PANTALLA_MS - transcurrido);

  // 6) Dormir
  irADormir(inicioUs);
}

void loop() {
  // Nunca se ejecuta: el ESP32 duerme al final de setup()
}
// FIN DEL CODIGO