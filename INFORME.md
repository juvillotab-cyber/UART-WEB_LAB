# Laboratorio: Adquisición de ADC vía UART y Visualización Web en Tiempo Real

**Integrantes:** 

María José Cuadros  
Miguel Angel Estrada  
Ever Daniel Hernández  
Juan David Villota

## 1. Descripción General

Este laboratorio implementa un sistema de adquisición de señales analógicas desde un **ESP32-C6** que lee un ADC periódicamente, envía los datos por **UART** a una **Raspberry Pi**, la cual ejecuta un servidor **Flask + Socket.IO** que retransmite los valores a un cliente web para visualización en tiempo real.

El objetivo principal es analizar la **frecuencia máxima alcanzable en el navegador** comparada con la frecuencia de adquisición del microcontrolador, identificando los **cuellos de botella** en la cadena de transmisión.

---

## 2. Arquitectura del Sistema

```
┌─────────────────────┐     UART      ┌──────────────────────┐   WebSocket   ┌──────────────┐
│     ESP32-C6        │ ────────────▶ │  Raspberry Pi (Flask)│ ────────────▶ │  Navegador   │
│                     │  921600 baud  │                      │               │              │
│ Timer → ADC → UART  │               │  serial → socketio   │               │  Canvas RT   │
└─────────────────────┘               └──────────────────────┘               └──────────────┘
      5000 Hz                               ~2000 Hz                              ~60 fps
```

| Componente | Rol | Tecnología |
|---|---|---|
| **ESP32-C6** | Adquisición ADC y transmisión UART | ESP-IDF 5.5.4, FreeRTOS |
| **Raspberry Pi** | Servidor web + bridge UART→WebSocket | Python, Flask, Flask-SocketIO |
| **Navegador** | Visualización en tiempo real | HTML5 Canvas, Socket.IO client |

El ADC del ESP32-C6 (GPIO0) se conecta a un potenciómetro como divisor de tensión entre 3.3 V y GND, con una resistencia de 10 kΩ en serie. El voltaje en el pin varía de 0 a 3.3 V, digitalizado a 12 bits (0–4095).

---

## 3. Implementación del ESP32-C6

### 3.1. Configuración del ADC

- **Unidad**: ADC1, Canal 0 (GPIO0)
- **Resolución**: 12 bits (0–4095), Atenuación: 12 dB (0–3300 mV)
- **Calibración**: Curva de ajuste (curve fitting) habilitada

### 3.2. Temporizador de Muestreo

```c
#define SAMPLE_PERIOD_US    200   // 200 µs → 5000 Hz
```

Timer con resolución de 1 MHz (1 tick = 1 µs). La interrupción notifica a la tarea de muestreo mediante `vTaskNotifyGiveFromISR()`.

### 3.3. Trama UART

Cada muestra se serializa como **JSON** con terminación `\n`:

```
{"v":<valor_mV>}\n   →   8–11 bytes (11 bytes para valor de 4 dígitos como 1234)
```

### 3.4. Configuración UART

| Parámetro | Valor |
|---|---|
| Puerto | `UART_NUM_1`, TX=GPIO2, RX=GPIO3 |
| **Baudrate** | **921600** |
| Data/Paridad/Stop | 8 / Ninguna / 1 |
| Buffer | 256 bytes |

---

## 4. Implementación del Servidor (Raspberry Pi)

### 4.1. Servicio Flask-SocketIO (`service.py`)

- Abre `/dev/ttyAMA3` a 921600 baud
- Hilo separado para leer UART: `ser.readline()` → `.decode().strip()` → `socketio.emit()`
- `async_mode = "threading"` (hilos nativos, GIL presente)

### 4.2. Frontend Web (`index.html`)

- Canvas con `desynchronized: true` para minimizar latencia de pintado
- Buffer circular de 4096 muestras (`Float32Array`)
- Cálculo de frecuencia instantánea y promedio (ventana de 500 muestras)
- Loop de render con `requestAnimationFrame` (~60 fps)

---

## 5. Análisis de la Trama UART

Cada muestra del ADC (12 bits) se envía como una cadena JSON de la forma `{"v":<valor>}\n`, ocupando de 8 a 11 bytes. Para un valor típico de 4 dígitos como `{"v":1234}\n` son **11 bytes**.

En la transmisión UART, cada byte agrega 2 bits de framing (start + stop), por lo que cada byte representa 10 bits en el cable. Una trama de 11 bytes son **110 bits totales**.

A 921600 baud, el throughput máximo teórico es:
- **92,160 bytes/s** (921600 / 10)
- **8,378 muestras/s** (92,160 / 11)

La eficiencia es baja: solo **~10.9%** de los bits transportan información útil del ADC (12 bits de 110), debido al overhead del framing UART y la representación ASCII del JSON.

---

## 6. Resultados Experimentales

| Parámetro | Valor |
|---|---|
| Período del timer ESP32-C6 | 200 µs |
| **Frecuencia de muestreo ESP32-C6** | **5000 Hz** |
| Baudrate UART | 921600 |
| **Frecuencia máxima en navegador** | **~2000 Hz** |
| Relación web/ESP | **40%** |

- Con `SAMPLE_PERIOD_US = 200` (5000 Hz), el navegador mostraba ≈ 2000 Hz.
- Al reducir el período, la frecuencia en el web se estabilizaba en ~2000 Hz.
- Con períodos mayores (ej. 500 µs), el web reflejaba fielmente la frecuencia del ESP.

---

## 7. Análisis del Cuello de Botella

### 7.1. Cadena Completa

```
ESP32: Timer → ADC → snprintf → uart_write_bytes  →  UART (921600 baud)
RPi:    ser.readline() → .decode() → .strip() → socketio.emit()  →  WebSocket
Browser: socket.on("uart_data") → JSON.parse → pushSample() → draw()
```

### 7.2. Tiempos por Etapa

| Etapa | Tiempo | Frec. máxima |
|---|---|---|
| ADC oneshot | ~4 µs | 250 kHz |
| UART (11 bytes @ 921600) | ~119 µs | 8378 Hz |
| Python ser.readline + parse | ~200–300 µs | 3300–5000 Hz |
| **Python socketio.emit()** | **~200–500 µs** | **2000–5000 Hz** |
| Canvas render | 16.67 ms | 60 Hz |

### 7.3. Identificación

El cuello de botella está en el servidor Python por:

1. **`ser.readline()`**: Lee byte a byte hasta `\n`, con overhead del intérprete y cambios de contexto (~11 syscalls por línea).
2. **GIL**: El hilo UART y Flask compiten por el GIL; `socketio.emit()` bloquea la lectura.
3. **Socket.IO en modo threading**: Cada `emit` bloquea durante serialización y transmisión WebSocket.

**Tiempo total estimado: ~420–570 µs por muestra → ~2000 Hz máximo**, coincidiendo con lo observado.

### 7.4. Análisis del Kernel Linux (Raspberry Pi OS Lite)

La Raspberry Pi 4 ejecuta Raspberry Pi OS Lite con un kernel de propósito general. Los datos UART atraviesan varias capas antes de llegar a Python, y cada una impone un límite:

**Pin RX → FIFO hardware PL011 (16 bytes) → IRQ → Buffer del kernel (4096 bytes) → Python**

1. **FIFO hardware de solo 16 bytes**: El UART PL011 de la RPi4 tiene un buffer interno muy pequeño. A 921600 baud se llena en ~174 µs. El kernel de Raspberry Pi OS usa `PREEMPT_VOLUNTARY` (no es tiempo real), por lo que atender la interrupción puede demorar entre 10 y 500 µs. Si la interrupción llega tarde, el FIFO se desborda y se pierden muestras. Este es el primer filtro que impide que lleguen todas las muestras a 5000 Hz.

2. **Buffer del kernel de 4096 bytes**: A 5000 Hz, el ESP32 genera ~55,000 bytes/s. El buffer interno del kernel se llena en ~74 ms. Si Python no alcanza a consumir los datos antes de que se llene, el kernel descarta los nuevos. A 2000 Hz la tasa baja a ~22,000 bytes/s, dando ~186 ms de margen, que es suficiente para que el sistema se estabilice.

3. **Planificador del sistema (`HZ=250`)**: El kernel divide el tiempo en ticks de 4 ms. Python compite por CPU con otros procesos (red, SSH, etc.). Si el sistema pausa Python por unos pocos ticks (8–12 ms), se acumulan datos en el buffer. A 5000 Hz, el buffer se desborda rápido; a 2000 Hz hay suficiente margen para absorber estas pausas sin pérdida.

**Jerarquía de limitaciones:**

```
FIFO del chip PL011 (16 bytes) → se llena en 174 µs
    ↓
Latencia de interrupción del kernel → 10–500 µs (overrun si tarda)
    ↓
Buffer del kernel (4096 bytes) → se llena en 74 ms a 5000 Hz
    ↓
Planificador (tick cada 4 ms) → pausas de 4–12 ms
    ↓
Python (GIL + readline + emit) → ~500 µs/muestra → ~2000 Hz
```

**En la práctica**: a 5000 Hz el sistema operativo no logra mantener el ritmo y pierde datos en las capas del kernel. A 2000 Hz hay suficiente margen en cada capa, por lo que el sistema es estable. Por eso el navegador nunca supera los ~2000 Hz, incluso si el ESP32 muestrea más rápido.

---

## 8. Posibles Optimizaciones

### 8.1. Lado Servidor

| Optimización | Impacto |
|---|---|
| Eliminar `print()` del bucle | +10–20% |
| Usar `eventlet` o `gevent` | +50–100% |
| Leer en lotes (batch reads) | +100–200% |
| Formato binario en vez de JSON | +100–200% |

### 8.2. Lado ESP32-C6

| Optimización | Impacto |
|---|---|
| Trama binaria (4 bytes) en vez de JSON (11 bytes) | -82% tamaño |
| DMA para UART y ADC | Menos CPU overhead |
| Agrupar múltiples muestras por trama | Reduce overhead de framing |

---

## 9. Conclusiones

1. **Frecuencia máxima en web: ~2000 Hz** (limitada por kernel Linux + Python).
2. **Frecuencia de adquisición ESP32-C6: 5000 Hz**.
3. **El UART no es el cuello de botella**: soporta hasta ~8378 muestras/s a 921600 baud.
4. **Dos cuellos de botella en cascada**:
   - **Kernel**: FIFO PL011 (16 bytes) + IRQ + planificador CFS causan overruns a altas frecuencias.
   - **Aplicación**: Python con GIL, `readline()` y `socketio.emit()` limitan a ~2000 Hz.
5. **Eficiencia de trama baja**: solo ~10.9% de los bits son datos útiles del ADC.
6. Para superar 2000 Hz se requiere optimizar en múltiples niveles: prioridad de proceso, lectura por lotes, servidor asíncrono y trama binaria.

---

## 10. Estructura de Archivos del Proyecto

```
UART-WEB_LAB/
├── .gitignore                    ← Reglas de ignorado para Git
├── INFORME.md                    ← Este documento
│
├── firmware/                     ← Firmware ESP32-C6 (ESP-IDF)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   ├── .clangd
│   ├── .devcontainer/
│   ├── .vscode/
│   └── main/
│       ├── CMakeLists.txt
│       └── main.c                ← Código principal del ESP32-C6
│
└── server/                       ← Servidor Raspberry Pi (Flask)
    ├── requirements.txt          ← Dependencias Python
    ├── service.py                ← Servidor Flask + SocketIO
    └── templates/
        └── index.html            ← Frontend web con Canvas
```
