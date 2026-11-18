import cv2
import numpy as np
import socket
import threading
import time

# --- IP FIJA DEL ESP32 EN MODO ACCESS POINT ---
IP_ESP32 = "192.168.4.1"

ultimo_frame = None
lock_frame = threading.Lock()
ejecutando = True

estado_esp32 = "MOVIENDO"
decision_enviada = False
frames_ok_consecutivos = 0
FRAMES_CONFIRMACION = 5  # Exige 5 frames seguidos viendo la caja válida antes de abrir

def lector_stream_tcp():
    """Lee el video MJPEG del ESP32 por socket TCP directo (puerto 81)"""
    global ultimo_frame, ejecutando
    while ejecutando:
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3.0)
            sock.connect((IP_ESP32, 81))
            peticion = f"GET /stream HTTP/1.1\r\nHost: {IP_ESP32}\r\nConnection: keep-alive\r\n\r\n"
            sock.sendall(peticion.encode())

            bytes_buffer = b""
            while ejecutando:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                bytes_buffer += chunk
                inicio = bytes_buffer.find(b'\xff\xd8')
                fin = bytes_buffer.find(b'\xff\xd9')
                if inicio != -1 and fin != -1 and fin > inicio:
                    jpg = bytes_buffer[inicio:fin+2]
                    bytes_buffer = bytes_buffer[fin+2:]
                    img = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
                    if img is not None:
                        with lock_frame:
                            ultimo_frame = img
        except Exception:
            time.sleep(0.5)
        finally:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass

def consultar_tcp(ruta):
    """Envía peticiones HTTP rápidas al ESP32 por socket TCP (puerto 80)"""
    sock = None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(1.0)
        sock.connect((IP_ESP32, 80))
        req = f"GET {ruta} HTTP/1.1\r\nHost: {IP_ESP32}\r\nConnection: close\r\n\r\n"
        sock.sendall(req.encode())
        resp = sock.recv(512).decode(errors="ignore")
        if "\r\n\r\n" in resp:
            return resp.split("\r\n\r\n", 1)[1].strip()
    except Exception:
        pass
    finally:
        if sock:
            try:
                sock.close()
            except Exception:
                pass
    return ""

def monitor_ultrasonico():
    """Consulta periódicamente si el HC-SR04 detuvo la faja"""
    global estado_esp32, decision_enviada, frames_ok_consecutivos, ejecutando
    while ejecutando:
        resp = consultar_tcp("/estado")
        if resp in ("INSPECCIONAR", "MOVIENDO"):
            estado_esp32 = resp
            if resp == "MOVIENDO":
                decision_enviada = False
                frames_ok_consecutivos = 0
        time.sleep(0.15)

def nada(x):
    pass

# Iniciar hilos de comunicación en paralelo
threading.Thread(target=lector_stream_tcp, daemon=True).start()
threading.Thread(target=monitor_ultrasonico, daemon=True).start()

# Ventana de calibración en tiempo real
cv2.namedWindow("Controles")
cv2.createTrackbar("Brillo Caja", "Controles", 130, 255, nada)
cv2.createTrackbar("Area Minima", "Controles", 2500, 20000, nada)

print("Conectando con ESP32 (192.168.4.1)... Presiona 'q' para salir.")

try:
    while True:
        with lock_frame:
            frame = ultimo_frame.copy() if ultimo_frame is not None else None

        if frame is None:
            cv2.waitKey(10)
            continue

        umbral = cv2.getTrackbarPos("Brillo Caja", "Controles")
        area_min = cv2.getTrackbarPos("Area Minima", "Controles")

        # 1. Preprocesamiento y segmentación por brillo
        gris = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        blur = cv2.GaussianBlur(gris, (7, 7), 0)
        _, mascara = cv2.threshold(blur, umbral, 255, cv2.THRESH_BINARY)

        # Limpieza morfológica para eliminar ruido
        kernel = np.ones((7, 7), np.uint8)
        mascara = cv2.morphologyEx(mascara, cv2.MORPH_OPEN, kernel)
        mascara = cv2.morphologyEx(mascara, cv2.MORPH_CLOSE, kernel)

        contornos, _ = cv2.findContours(mascara, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        es_rectangulo_valido = False

        for cnt in contornos:
            area = cv2.contourArea(cnt)
            if area < area_min or area > (frame.shape[0] * frame.shape[1] * 0.85):
                continue

            # 2. Encuadre rotado y cálculo de rectangularidad
            rect_rotado = cv2.minAreaRect(cnt)
            caja_puntos = np.int32(cv2.boxPoints(rect_rotado))
            area_rect = rect_rotado[1][0] * rect_rotado[1][1]
            rectangularidad = float(area) / area_rect if area_rect > 0 else 0

            perimetro = cv2.arcLength(cnt, True)
            aprox = cv2.approxPolyDP(cnt, 0.04 * perimetro, True)
            vertices = len(aprox)

            # Criterio de calidad: 4 a 5 vértices y rectangularidad > 82%
            es_caja_sana = (4 <= vertices <= 5) and (rectangularidad > 0.82)
            x_txt, y_txt = caja_puntos[0]

            if es_caja_sana:
                es_rectangulo_valido = True
                color = (0, 255, 0)  # Verde
                texto = f"RECTANGULO OK ({rectangularidad*100:.0f}%)"
            else:
                color = (0, 0, 255)  # Rojo
                texto = f"DEFECTO / NO RECT ({rectangularidad*100:.0f}%)"

            cv2.drawContours(frame, [caja_puntos], 0, color, 3)
            cv2.putText(frame, texto, (max(x_txt, 10), max(y_txt - 10, 25)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)

        # 3. Decisión automática cuando el ultrasónico detiene la faja
        if estado_esp32 == "INSPECCIONAR" and not decision_enviada:
            if es_rectangulo_valido:
                frames_ok_consecutivos += 1
                if frames_ok_consecutivos >= FRAMES_CONFIRMACION:
                    decision_enviada = True
                    print(">>> CAJA APROBADA: Activando Servo 1 (MG996R)")
                    threading.Thread(target=consultar_tcp, args=("/abrir",), daemon=True).start()
            else:
                frames_ok_consecutivos = 0
                # Si no aprueba en 5 segundos, el ESP32 activará solo el Servo 2 (SG90)

        # 4. Textos de estado en pantalla
        if estado_esp32 == "INSPECCIONAR":
            cv2.putText(frame, "FAJA DETENIDA (HC-SR04) - EVALUANDO...", (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 255), 2)
        else:
            cv2.putText(frame, "FAJA EN MARCHA - ESPERANDO OBJETO", (10, 25),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 2)

        cv2.imshow("1. Mascara Binaria (Calibracion)", mascara)
        cv2.imshow("2. Estacion de Inspeccion", frame)

        if cv2.waitKey(15) & 0xFF == ord('q'):
            break
finally:
    ejecutando = False
    cv2.destroyAllWindows()
