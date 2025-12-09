# ESP32-S3 Dual-Core MIDI Step Sequencer

Un secuenciador MIDI de 8 pasos optimizado para ESP32-S3 con arquitectura de doble núcleo, diseñado para sincronización perfecta con DAWs como Ableton Live.

## Características

- **8 pasos programables** con control individual de nota, velocidad, duración y canal MIDI
- **Sincronización MIDI Clock** como esclavo de DAW (24 PPQN)
- **Arquitectura dual-core** para máximo rendimiento:
  - **Core 0**: Procesamiento MIDI en tiempo real y motor del secuenciador
  - **Core 1**: Interfaz de usuario y feedback visual
- **Interfaz táctil completa** con encoders rotativos y pulsadores
- **Feedback visual** mediante pantalla OLED y tira de LEDs NeoPixel
- **USB MIDI nativo** usando TinyUSB

## Hardware Requerido

### Microcontrolador
- ESP32-S3 Dev Kit

### Componentes
- **Multiplexor**: CD74HC4067 (16 canales)
- **Encoders**: 4x Encoder rotativo con pulsador integrado
- **Pantalla**: OLED 0.96" (128x64, SSD1306, I2C)
- **LEDs**: Tira NeoPixel WS2812B (8 LEDs)
- **Pulsadores**: 8x para control de pasos

### Conexiones de Pines

#### Multiplexor CD74HC4067
- **SIG**: Pin 39 (INPUT_PULLUP)
- **S0**: Pin 35
- **S1**: Pin 36
- **S2**: Pin 37
- **S3**: Pin 38

#### Encoders Rotativos
| Encoder | Pin A | Pin B | Función |
|---------|-------|--------|---------|
| 1 | 5 | 6 | Modo NOTE |
| 2 | 7 | 17 | Modo VELOCITY |
| 3 | 16 | 15 | Modo DURATION |
| 4 | 8 | 3 | Modo CHANNEL |

#### Pantalla OLED I2C
- **SDA**: Pin 9
- **SCL**: Pin 10
- **Dirección**: 0x3C

#### Tira NeoPixel
- **DIN**: Pin 47
- **Cantidad**: 8 LEDs

#### Mapeo del Multiplexor
| Canal | Función |
|-------|---------|
| C15-C8 | Pulsadores pasos 1-8 |
| C3 | Pulsador Encoder 1 (NOTE) |
| C2 | Pulsador Encoder 2 (VELOCITY) |
| C1 | Pulsador Encoder 3 (DURATION) |
| C0 | Pulsador Encoder 4 (CHANNEL) |

## Librerías Necesarias

```cpp
#include <Adafruit_TinyUSB.h>     // USB MIDI
#include <MIDI.h>                 // MIDI Library by Forty Seven Effects
#include <Wire.h>                 // I2C
#include <Adafruit_GFX.h>         // Gráficos base
#include <Adafruit_SSD1306.h>     // Pantalla OLED
#include <Adafruit_NeoPixel.h>    // LEDs WS2812B
#include <ESP32Encoder.h>         // Encoders rotativos
```

## Funcionamiento

### Sincronización MIDI
- El secuenciador actúa como **esclavo** del DAW
- Responde a mensajes MIDI Clock (0xF8), Start (0xFA), Stop (0xFC), Continue (0xFB)
- **6 clocks por paso** (resolución de semicorchea a 24 PPQN)
- Solo se ejecuta cuando recibe Start/Continue del DAW

### Controles

#### Pulsadores de Pasos (1-8)
- **Función**: Toggle del estado activo/inactivo de cada paso
- **Feedback**: LED azul para pasos activos, LED rojo para paso actual

#### Pulsadores de Encoder (Modos de Edición)
- **Encoder 1**: Modo NOTE (selección de nota MIDI 0-127)
- **Encoder 2**: Modo VELOCITY (velocidad 1-127)
- **Encoder 3**: Modo DURATION (duración 20-2000ms, incrementos de 5ms)
- **Encoder 4**: Modo CHANNEL (canal MIDI 1-16)

#### Encoders Rotativos
- Modifican el parámetro del modo activo para el **paso actual** (que está sonando)
- Resolución configurable por detent (default: 4 ticks por detent)

### Pantalla OLED
La pantalla muestra:
- **Línea 1**: Estado (PLAYING/STOPPED) + Modo activo (NOTE/VEL/DUR/CHAN)
- **Visualización de pasos**: 8 recuadros representando cada paso
  - Recuadro lleno: paso actual
  - Recuadro con punto: paso activo
  - Recuadro vacío: paso inactivo
- **Parámetros del paso actual**:
  - Note: número de nota MIDI
  - Vel: velocidad
  - Dur: duración en milisegundos
  - Chan: canal MIDI

### LEDs NeoPixel
- **LED Rojo**: Paso que está sonando actualmente
- **LED Azul**: Pasos activos pero no sonando
- **LED Apagado**: Pasos inactivos

## Arquitectura del Software

### Core 0 (Tiempo Real - Máxima Prioridad)
- Procesamiento USB MIDI
- Manejo de callbacks MIDI en tiempo real
- Motor del secuenciador (avance de pasos)
- Programación de Note Off diferidos
- **Restricción**: Sin operaciones de bloqueo

### Core 1 (Interfaz de Usuario - Prioridad Normal)
- Escaneo de pulsadores con debounce
- Lectura de encoders rotativos
- Actualización de pantalla OLED (25 FPS)
- Actualización de LEDs (50 FPS)
- Lógica de interfaz de usuario

### Comunicación Inter-Core
- Variables `volatile` protegidas por mutex críticos
- `portENTER_CRITICAL` / `portEXIT_CRITICAL` para sincronización
- Funciones helper para copia segura de structs volátiles

## Configuración Inicial

### Secuencia por Defecto
- **8 pasos activos**
- **Notas**: Escala cromática desde Do central (60, 61, 62... 67)
- **Velocidad**: 100
- **Duración**: 150ms
- **Canal**: 1

### Parámetros Configurables
```cpp
constexpr uint8_t CLOCKS_PER_STEP = 6;        // 6 clocks = 1/16 nota
constexpr uint32_t BUTTON_DEBOUNCE_MS = 15;   // Debounce de pulsadores
constexpr int32_t ENCODER_TICKS_PER_DETENT = 4; // Resolución de encoder
```

## Uso con DAW

1. **Conectar** el ESP32-S3 por USB al ordenador
2. **Configurar** el DAW para enviar MIDI Clock al dispositivo
3. **Activar** Transport en el DAW (Play)
4. El secuenciador comenzará automáticamente sincronizado

### Configuración en Ableton Live
1. Preferences → Link/Tempo/MIDI
2. MIDI Ports: Habilitar "ESP32-S3 Dual-Core Sequencer" como salida
3. Sync: Activar para el dispositivo
4. Presionar Play en Ableton

## Características Técnicas

- **Latencia MIDI**: Mínima (procesamiento en Core 0)
- **Resolución temporal**: 1ms (FreeRTOS tick)
- **Capacidad de Note Off**: 8 notas simultáneas máximo
- **Memoria**: Optimizada para ESP32-S3
- **Debounce**: 15ms por software
- **Actualización de display**: 40ms (25 FPS)
- **Actualización de LEDs**: 20ms (50 FPS)

## Desarrollo y Personalización

### Modificar Número de Pasos
```cpp
constexpr uint8_t NUM_STEPS = 16; // Cambiar de 8 a 16 pasos
```

### Ajustar Timing
```cpp
constexpr uint8_t CLOCKS_PER_STEP = 12; // Cambiar a 1/8 nota
```

### Personalizar Colores LED
```cpp
color = leds.Color(0, 255, 0); // Verde para pasos activos
```

## Solución de Problemas

### El secuenciador no se inicia
- Verificar que el DAW envía MIDI Clock
- Confirmar mensaje de Start desde el DAW

### Encoders no responden
- Verificar conexiones de cuadratura
- Ajustar `ENCODER_TICKS_PER_DETENT`

### LEDs no se encienden
- Verificar alimentación de la tira NeoPixel
- Confirmar pin de datos (Pin 47)

### Pantalla en blanco
- Verificar conexiones I2C (SDA/SCL)
- Confirmar dirección I2C (0x3C)

## Licencia

Este proyecto está basado en el ejemplo oficial de Adafruit para TinyUSB MIDI y utiliza las siguientes librerías open source bajo licencia MIT.

## Créditos

- Basado en código de ejemplo de Adafruit Industries
- MIDI Library by Forty Seven Effects
- Arquitectura dual-core optimizada para ESP32-S3