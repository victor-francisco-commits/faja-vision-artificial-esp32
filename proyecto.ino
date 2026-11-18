#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "esp_http_server.h"

// 1. RED WI-FI PROPIA DEL ESP32 (Access Point)
const char* ap_ssid = "Banda_Control_Calidad";
const char* ap_pass = "12345678";

// 2. PINES DE CÁMARA ESP32-WROVER-KIT
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

// 3. PINES DE ACTUADORES Y SENSOR ULTRASÓNICO
const int SERVO1_PIN = 2;    // IO2  -> Servo 1 MG996R (Compuerta principal)
const int SERVO2_PIN = 12;   // IO12 -> Servo 2 SG90 (Actuador auxiliar de respaldo)

const int ENA_PIN    = 14;   // IO14 -> Velocidad L298N
const int IN1_PIN    = 13;   // IO13 -> Giro L298N
const int IN2_PIN    = 15;   // IO15 -> Giro L298N
const int MOTOR_CH   = 14;

const int TRIG_PIN   = 32;   // IO32 -> TRIG HC-SR04
const int ECHO_PIN   = 33;   // IO33 -> ECHO HC-SR04 (con divisor de voltaje)

// Parámetros calibrables
const float DISTANCIA_DETECCION_CM = 12.0;
const int VELOCIDAD_FAJA = 180;
const unsigned long TIEMPO_ESPERA_MAX_MS = 5000;      // 5s esperando decisión de cámara
const unsigned long TIEMPO_DESPACHO_SERVO1_MS = 5000; // 5s corriendo faja con Servo 1 abierto a 90°

// Control independiente de ambos servos (sin temblor)
volatile int pulsoServo1Us = 500;
volatile int pulsosRestantes1 = 28;

volatile int pulsoServo2Us = 500;
volatile int pulsosRestantes2 = 28;

// Estados del ciclo:
// 0 = Faja avanzando buscando objeto
// 1 = Objeto detectado (Faja detenida esperando Servo 1 o timeout de 5s)
// 2 = Ejecutando movimiento completo del Servo 2 (SG90)
// 3 = Despachando caja con Servo 1 abierto a 90° y faja corriendo durante 5 segundos
int estadoFaja = 0;
unsigned long tiempoDetencion = 0;
unsigned long tiempoAccionSG90 = 0;
unsigned long tiempoInicioDespacho = 0;
int faseSG90 = 0;

WebServer server(80);
httpd_handle_t stream_httpd = NULL;

// --- TAREA DEDICADA PARA AMBOS SERVOS ---
void tareaServos(void * parameter) {
  pinMode(SERVO1_PIN, OUTPUT);
  pinMode(SERVO2_PIN, OUTPUT);
  digitalWrite(SERVO1_PIN, LOW);
  digitalWrite(SERVO2_PIN, LOW);

  while (true) {
    if (pulsosRestantes1 > 0) {
      int p1 = pulsoServo1Us;
      digitalWrite(SERVO1_PIN, HIGH);
      delayMicroseconds(p1);
      digitalWrite(SERVO1_PIN, LOW);
      pulsosRestantes1--;
    }
    if (pulsosRestantes2 > 0) {
      int p2 = pulsoServo2Us;
      digitalWrite(SERVO2_PIN, HIGH);
      delayMicroseconds(p2);
      digitalWrite(SERVO2_PIN, LOW);
      pulsosRestantes2--;
    }
    vTaskDelay(pdMS_TO_TICKS(18));
  }
}

void moverFaja(bool encender) {
  if (encender) {
    digitalWrite(IN1_PIN, HIGH);
    digitalWrite(IN2_PIN, LOW);
    ledcWrite(MOTOR_CH, VELOCIDAD_FAJA);
  } else {
    digitalWrite(IN1_PIN, LOW);
    digitalWrite(IN2_PIN, LOW);
    ledcWrite(MOTOR_CH, 0);
  }
}

void moverServo1(int us) {
  pulsoServo1Us = us;
  pulsosRestantes1 = 28;
}

void moverServo2(int us) {
  pulsoServo2Us = us;
  pulsosRestantes2 = 28;
}

float medirDistanciaCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duracion = pulseIn(ECHO_PIN, HIGH, 20000);
  if (duracion == 0) return 999.0;
  return (duracion * 0.0343) / 2.0;
}

// --- STREAM DE VIDEO MJPEG (Puerto 81) ---
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];
  res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part_buf, 64, "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      res = httpd_resp_send_chunk(req, "--frame\r\n", 9);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;
  }
  return res;
}

void iniciarStream() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  xTaskCreatePinnedToCore(tareaServos, "ServosTask", 2048, NULL, 2, NULL, 0);

  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  ledcSetup(MOTOR_CH, 1000, 8);
  ledcAttachPin(ENA_PIN, MOTOR_CH);

  moverServo1(500); // MG996R en 0°
  moverServo2(500); // SG90 en 0°
  moverFaja(true);  // Faja en marcha

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Error iniciando camara");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);
  delay(200);
  Serial.print("IP Fija: "); Serial.println(WiFi.softAPIP());

  // Python consulta si la faja está detenida esperando análisis
  server.on("/estado", []() {
    if (estadoFaja == 1) {
      server.send(200, "text/plain", "INSPECCIONAR");
    } else {
      server.send(200, "text/plain", "MOVIENDO");
    }
  });

  // Python detectó rectángulo válido -> Abre Servo 1 a 90° y corre faja 5 segundos
  server.on("/abrir", []() {
    if (estadoFaja == 1) {
      Serial.println(">>> SERVO 1 (MG996R) ABIERTO A 90° -> Faja corriendo por 5 segundos...");
      moverServo1(1500); // Gira a 90°
      moverFaja(true);   // Arranca faja
      estadoFaja = 3;
      tiempoInicioDespacho = millis();
    }
    server.send(200, "text/plain", "OK_ABRIR");
  });

  server.begin();
  iniciarStream();
}

void loop() {
  server.handleClient();

  static unsigned long ultimaLectura = 0;
  if (millis() - ultimaLectura > 80) {
    ultimaLectura = millis();
    float dist = medirDistanciaCm();

    // ESTADO 0: Faja avanzando hasta que el HC-SR04 detecte el objeto
    if (estadoFaja == 0 && dist > 1.0 && dist <= DISTANCIA_DETECCION_CM) {
      Serial.printf("Objeto detectado a %.1f cm -> FAJA DETENIDA (Esperando Servo 1 o 5s)\n", dist);
      moverFaja(false);
      tiempoDetencion = millis();
      estadoFaja = 1;
    }

    // ESTADO 1: Faja detenida esperando orden /abrir para Servo 1.
    // Si pasan 5 segundos sin que Servo 1 se mueva -> Activa Servo 2 (SG90)
    else if (estadoFaja == 1) {
      if (millis() - tiempoDetencion >= TIEMPO_ESPERA_MAX_MS) {
        Serial.println("!!! 5s cumplidos sin Servo 1 -> ACTIVANDO SERVO 2 (SG90)");
        moverServo2(1500); // SG90 gira a 90°
        tiempoAccionSG90 = millis();
        faseSG90 = 1;
        estadoFaja = 2;
      }
    }

    // ESTADO 2: Ejecutando recorrido completo del SG90 antes de reiniciar el ciclo
    else if (estadoFaja == 2) {
      if (faseSG90 == 1 && (millis() - tiempoAccionSG90 >= 800)) {
        moverServo2(500); // SG90 regresa a 0°
        tiempoAccionSG90 = millis();
        faseSG90 = 2;
      } 
      else if (faseSG90 == 2 && (millis() - tiempoAccionSG90 >= 800)) {
        Serial.println("<<< SG90 termino su movimiento -> Reanudando faja por 5s para reiniciar ciclo");
        moverFaja(true);
        tiempoInicioDespacho = millis();
        estadoFaja = 3;
      }
    }

    // ESTADO 3: Faja corriendo durante 5 segundos con Servo 1 activado en 90°.
    // Al cumplirse los 5 segundos, regresa el servo a 0° y reinicia el ciclo.
    else if (estadoFaja == 3) {
      if (millis() - tiempoInicioDespacho >= TIEMPO_DESPACHO_SERVO1_MS) {
        Serial.println("=== 5s COMPLETADOS: Cerrando Servo 1 (0°) y reiniciando ciclo ===");
        moverServo1(500); // Regresa Servo 1 a 0°
        moverServo2(500); // Asegura Servo 2 en 0°
        estadoFaja = 0;   // Reinicia ciclo (faja sigue avanzando buscando el siguiente objeto)
      }
    }
  }
  delay(5);
}