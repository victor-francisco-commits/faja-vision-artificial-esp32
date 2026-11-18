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
