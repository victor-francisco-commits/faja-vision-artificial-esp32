# 🏭 Faja Transportadora Automatizada con Control de Calidad por Visión Artificial

Sistema mecatrónico de clasificación en tiempo real que integra un microcontrolador **ESP32-WROVER-DEV (CAM)** en modo Access Point y procesamiento digital de imágenes en **Python + OpenCV** para detectar, inspeccionar y separar objetos según su geometría y rectangularidad.

---

## 📋 Descripción del Sistema

El sistema opera bajo un ciclo cerrado de inspección y actuación:
1. **Detección de presencia:** Un sensor ultrasónico **HC-SR04** detecta la llegada de un objeto a $\le 12.0\text{ cm}$ y detiene la faja transportadora accionada por un motor DC con driver **L298N**.
2. **Adquisición de video:** El ESP32 transmite video MJPEG en tiempo real mediante un servidor HTTP dedicado (puerto `81`) a través de su propia red Wi-Fi (`192.168.4.1`).
3. **Inspección por visión artificial:** Un cliente en Python recibe el flujo TCP, segmenta el objeto por umbralización de brillo, limpia ruido morfológico y calcula su **rectangularidad** y número de vértices.
4. **Clasificación automática:**
   * **Producto Aprobado (Rectángulo válido $\ge 82\%$ en 5 frames consecutivos):** Python envía la petición `/abrir` al ESP32, activando el **Servo 1 (MG996R)** a $90^\circ$ y reanudando la faja durante $5\text{ s}$ para su despacho.
   * **Producto Rechazado / Timeout ($5\text{ s}$ sin aprobación):** El ESP32 ejecuta automáticamente la secuencia de descarte mediante el **Servo 2 (SG90)** antes de reiniciar el ciclo.

---

## ⚙️ Arquitectura y Flujo de Estados

```text
[Faja en Marcha (Estado 0)] 
       │
       ▼  (HC-SR04 detecta objeto <= 12 cm)
[Faja Detenida - Esperando Evaluación (Estado 1)]
       │
       ├──► (Python valida rectángulo OK x5 frames -> GET /abrir)
       │         └──► [Servo 1 MG996R a 90° + Faja 5s (Estado 3)] ──► Reinicia Ciclo
       │
       └──► (Timeout 5s sin aprobación)
                 └──► [Secuencia Servo 2 SG90 (Estado 2)] ──► [Faja 5s (Estado 3)] ──► Reinicia Ciclo

```

---

## 🔌 Hardware y Pinout (ESP32-WROVER-DEV)

### Actuadores y Sensores

| Componente | Pin ESP32 | Función |
| --- | --- | --- |
| **Servo 1 (MG996R)** | `GPIO 2` | Compuerta principal de despacho ($0^\circ \rightarrow 90^\circ$) |
| **Servo 2 (SG90)** | `GPIO 12` | Actuador auxiliar de rechazo / respaldo |
| **Driver L298N (ENA)** | `GPIO 14` | Control de velocidad PWM (`CH 14`, `1 kHz`, 8-bit) |
| **Driver L298N (IN1 / IN2)** | `GPIO 13` / `GPIO 15` | Control de sentido de giro del motor DC |
| **Sensor HC-SR04 (TRIG)** | `GPIO 32` | Disparo de pulso ultrasónico |
| **Sensor HC-SR04 (ECHO)** | `GPIO 33` | Recepción de eco (con divisor de voltaje a $3.3\text{ V}$) |

### Cámara (ESP32-WROVER-KIT)

* **Control:** `SIOD: 26`, `SIOC: 27`, `XCLK: 21`, `VSYNC: 25`, `HREF: 23`, `PCLK: 22`
* **Bus de datos (D0–D7):** `GPIO 4, 5, 18, 19, 36, 39, 34, 35`

> **Nota técnica:** El control PWM de ambos servomotores corre en una tarea independiente de FreeRTOS fijada al **Core 0** (`xTaskCreatePinnedToCore`) con generación de pulsos por microsegundos para evitar temblores (*jitter*) provocados por las interrupciones del Wi-Fi y la cámara.

---

## 📂 Estructura del Repositorio

```text
├── firmware/
│   └── control_faja_esp32.ino     # Código Arduino: Access Point, Stream MJPEG, WebServer y FreeRTOS
├── vision/
│   └── inspeccion_calidad.py      # Cliente Python: Sockets TCP, OpenCV y toma de decisiones
└── README.md

```

---

## 🚀 Instalación y Uso

### 1. Firmware (ESP32)

1. Abrir `firmware/control_faja_esp32.ino` en **Arduino IDE**.
2. Seleccionar la placa **ESP32 Wrover Module** y habilitar **PSRAM**.
3. Cargar el código. El ESP32 creará la red Wi-Fi:
* **SSID:** `Banda_Control_Calidad`
* **Contraseña:** `12345678`
* **IP Fija:** `192.168.4.1` (Puerto `80` para control HTTP, Puerto `81` para `/stream`).



### 2. Visión Artificial (Python)

1. Instalar las dependencias necesarias:
```bash
pip install opencv-python numpy

```


2. Conectar la PC a la red Wi-Fi `Banda_Control_Calidad`.
3. Ejecutar el script de inspección:
```bash
python vision/inspeccion_calidad.py

```


4. Ajustar los deslizadores en la ventana **Controles** (`Brillo Caja` y `Area Minima`) según la iluminación ambiental. Presionar `q` para finalizar.

---

## 🛠️ Tecnologías Utilizadas

* **Microcontrolador & Firmware:** C/C++, ESP32, FreeRTOS, `esp_camera.h`, `esp_http_server.h`
* **Visión Artificial & Concurrencia:** Python 3, OpenCV (`cv2`), NumPy, `socket`, `threading`

