#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Encoder.h>

// -----------------------------------------------------------------------------
// Hardware configuration
// -----------------------------------------------------------------------------
constexpr uint8_t MUX_SIG_PIN = 39;   // CD74HC4067 SIG
constexpr uint8_t MUX_S0_PIN  = 35;
constexpr uint8_t MUX_S1_PIN  = 36;
constexpr uint8_t MUX_S2_PIN  = 37;
constexpr uint8_t MUX_S3_PIN  = 38;

constexpr uint8_t ENCODER_PINS_A[4] = {5, 7, 16, 8};
constexpr uint8_t ENCODER_PINS_B[4] = {6, 17, 15, 3};

constexpr uint8_t NEOPIXEL_PIN   = 47;
constexpr uint8_t NEOPIXEL_COUNT = 8;

constexpr uint8_t OLED_SDA_PIN = 9;
constexpr uint8_t OLED_SCL_PIN = 10;
constexpr uint8_t OLED_RESET   = -1;  // Shared reset

constexpr uint32_t BUTTON_DEBOUNCE_MS = 15;
constexpr uint8_t CLOCKS_PER_STEP = 6;  // 24 PPQN / 4 (1/16 note)

// -----------------------------------------------------------------------------
// MIDI setup (reference: midi_test.ino communication pattern)
// -----------------------------------------------------------------------------
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

// -----------------------------------------------------------------------------
// Sequencer state
// -----------------------------------------------------------------------------
constexpr uint8_t NUM_STEPS = 8;

struct StepData {
  bool active;
  uint8_t note;
  uint8_t velocity;
  uint16_t duration_ms;
  uint8_t channel;
};

// Helper accessors to bridge volatile storage and non-volatile local copies.
static inline StepData readStep(const volatile StepData& src) {
  StepData copy;
  copy.active = src.active;
  copy.note = src.note;
  copy.velocity = src.velocity;
  copy.duration_ms = src.duration_ms;
  copy.channel = src.channel;
  return copy;
}

static inline void writeStep(volatile StepData& dest, const StepData& src) {
  dest.active = src.active;
  dest.note = src.note;
  dest.velocity = src.velocity;
  dest.duration_ms = src.duration_ms;
  dest.channel = src.channel;
}

volatile StepData steps[NUM_STEPS];
volatile bool is_playing = false;
volatile uint8_t current_step = 0;  // Currently sounding step
volatile uint8_t next_step = 0;     // Step that will trigger on next advance
volatile uint8_t clock_pulse_count = 0;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE stepsMux = portMUX_INITIALIZER_UNLOCKED;

enum EditMode : uint8_t { MODE_NOTE = 0, MODE_VELOCITY, MODE_DURATION, MODE_CHANNEL };
volatile EditMode current_mode = MODE_NOTE;

// -----------------------------------------------------------------------------
// Note-off scheduling (kept on Core 0 to guarantee timing)
// -----------------------------------------------------------------------------
struct PendingNoteOff {
  bool active;
  uint8_t note;
  uint8_t channel;
  uint32_t off_time_ms;
};

constexpr size_t MAX_PENDING_NOTES = 8;
PendingNoteOff pending_notes[MAX_PENDING_NOTES];

// -----------------------------------------------------------------------------
// UI helpers
// -----------------------------------------------------------------------------
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RESET);
Adafruit_NeoPixel leds(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

ESP32Encoder encoders[4];
long encoder_last_count[4] = {0, 0, 0, 0};
constexpr int32_t ENCODER_TICKS_PER_DETENT = 4;  // Depends on mechanical encoder

const uint8_t STEP_BUTTON_CHANNELS[NUM_STEPS] = {15, 14, 13, 12, 11, 10, 9, 8};
const uint8_t MODE_BUTTON_CHANNELS[4] = {3, 2, 1, 0};  // Encoder push-buttons

struct ButtonState {
  bool stable_state = true;  // Pull-up keeps idle HIGH
  bool last_reading = true;
  uint32_t last_change = 0;
};
ButtonState step_buttons[NUM_STEPS];
ButtonState mode_buttons[4];

// -----------------------------------------------------------------------------
// FreeRTOS task handles
// -----------------------------------------------------------------------------
TaskHandle_t midiTaskHandle = nullptr;
TaskHandle_t uiTaskHandle = nullptr;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void midiTask(void* parameter);
void uiTask(void* parameter);
void handleClock();
void handleStart();
void handleStop();
void handleContinue();
void advanceSequencerStep();
void scheduleNoteOff(uint8_t note, uint8_t channel, uint32_t duration_ms);
void processPendingNoteOffs();
void flushPendingNotes();
void initializeSteps();
void configureHardware();
bool readMuxChannel(uint8_t channel);
void selectMuxChannel(uint8_t channel);
void scanButtons();
void processStepButton(uint8_t index, bool pressed);
void processModeButton(uint8_t index, bool pressed);
void readEncoders();
void applyEncoderDelta(EditMode mode, int32_t delta);
void updateDisplay();
void updateLeds();
const __FlashStringHelper* modeToLabel(EditMode mode);

// -----------------------------------------------------------------------------
// MIDI callbacks (Core 0)
// -----------------------------------------------------------------------------
void handleClock() {
  if (!is_playing) {
    return;
  }

  uint8_t pulse;
  portENTER_CRITICAL_ISR(&stateMux);
  pulse = ++clock_pulse_count;
  portEXIT_CRITICAL_ISR(&stateMux);

  if (pulse >= CLOCKS_PER_STEP) {
    portENTER_CRITICAL_ISR(&stateMux);
    clock_pulse_count = 0;
    portEXIT_CRITICAL_ISR(&stateMux);
    advanceSequencerStep();
  }
}

void handleStart() {
  flushPendingNotes();
  portENTER_CRITICAL(&stateMux);
  is_playing = true;
  clock_pulse_count = 0;
  current_step = 0;
  next_step = 0;
  portEXIT_CRITICAL(&stateMux);
}

void handleStop() {
  portENTER_CRITICAL(&stateMux);
  is_playing = false;
  clock_pulse_count = 0;
  portEXIT_CRITICAL(&stateMux);
  flushPendingNotes();
}

void handleContinue() {
  portENTER_CRITICAL(&stateMux);
  is_playing = true;
  clock_pulse_count = 0;
  portEXIT_CRITICAL(&stateMux);
}

// -----------------------------------------------------------------------------
// Sequencer core logic (Core 0)
// -----------------------------------------------------------------------------
void advanceSequencerStep() {
  uint8_t step_to_play;

  portENTER_CRITICAL(&stateMux);
  step_to_play = next_step;
  current_step = step_to_play;
  next_step = (step_to_play + 1) % NUM_STEPS;
  portEXIT_CRITICAL(&stateMux);

  StepData snapshot;
  portENTER_CRITICAL(&stepsMux);
  snapshot = readStep(steps[step_to_play]);
  portEXIT_CRITICAL(&stepsMux);

  if (snapshot.active) {
    MIDI.sendNoteOn(snapshot.note, snapshot.velocity, snapshot.channel);
    scheduleNoteOff(snapshot.note, snapshot.channel, snapshot.duration_ms);
  }
}

void scheduleNoteOff(uint8_t note, uint8_t channel, uint32_t duration_ms) {
  const uint32_t off_time = millis() + duration_ms;
  for (PendingNoteOff& slot : pending_notes) {
    if (!slot.active) {
      slot.active = true;
      slot.note = note;
      slot.channel = channel;
      slot.off_time_ms = off_time;
      return;
    }
  }
  // If we run out of slots, send immediate note off to avoid hanging notes.
  MIDI.sendNoteOff(note, 0, channel);
}

void processPendingNoteOffs() {
  const uint32_t now = millis();
  for (PendingNoteOff& slot : pending_notes) {
    if (slot.active && static_cast<int32_t>(now - slot.off_time_ms) >= 0) {
      MIDI.sendNoteOff(slot.note, 0, slot.channel);
      slot.active = false;
    }
  }
}

void flushPendingNotes() {
  for (PendingNoteOff& slot : pending_notes) {
    if (slot.active) {
      MIDI.sendNoteOff(slot.note, 0, slot.channel);
      slot.active = false;
    }
  }
}

// -----------------------------------------------------------------------------
// Hardware helpers
// -----------------------------------------------------------------------------
void initializeSteps() {
  portENTER_CRITICAL(&stepsMux);
  for (uint8_t i = 0; i < NUM_STEPS; ++i) {
    steps[i].active = true;
    steps[i].note = 60 + i;       // Chromatic starting at Middle C
    steps[i].velocity = 100;
    steps[i].duration_ms = 150;
    steps[i].channel = 1;
  }
  portEXIT_CRITICAL(&stepsMux);
}

void configureHardware() {
  pinMode(MUX_SIG_PIN, INPUT_PULLUP);
  pinMode(MUX_S0_PIN, OUTPUT);
  pinMode(MUX_S1_PIN, OUTPUT);
  pinMode(MUX_S2_PIN, OUTPUT);
  pinMode(MUX_S3_PIN, OUTPUT);

  for (uint8_t i = 0; i < 4; ++i) {
    encoders[i].attachHalfQuad(ENCODER_PINS_A[i], ENCODER_PINS_B[i]);
    encoders[i].clearCount();
    encoder_last_count[i] = 0;
  }

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.display();

  leds.begin();
  leds.setBrightness(64);
  leds.show();

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  usb_midi.setStringDescriptor("ESP32-S3 Dual-Core Sequencer");
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  MIDI.setHandleClock(handleClock);
  MIDI.setHandleStart(handleStart);
  MIDI.setHandleStop(handleStop);
  MIDI.setHandleContinue(handleContinue);
}

// -----------------------------------------------------------------------------
// Multiplexer I/O
// -----------------------------------------------------------------------------
void selectMuxChannel(uint8_t channel) {
  digitalWrite(MUX_S0_PIN, (channel >> 0) & 0x01);
  digitalWrite(MUX_S1_PIN, (channel >> 1) & 0x01);
  digitalWrite(MUX_S2_PIN, (channel >> 2) & 0x01);
  digitalWrite(MUX_S3_PIN, (channel >> 3) & 0x01);
  delayMicroseconds(2);  // Allow the mux to settle
}

bool readMuxChannel(uint8_t channel) {
  selectMuxChannel(channel);
  return digitalRead(MUX_SIG_PIN);
}

// -----------------------------------------------------------------------------
// Button handling (Core 1)
// -----------------------------------------------------------------------------
void scanButtons() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < NUM_STEPS; ++i) {
    bool raw = readMuxChannel(STEP_BUTTON_CHANNELS[i]);
    ButtonState& state = step_buttons[i];
    if (raw != state.last_reading) {
      state.last_change = now;
      state.last_reading = raw;
    }
    if ((now - state.last_change) >= BUTTON_DEBOUNCE_MS) {
      if (raw != state.stable_state) {
        state.stable_state = raw;
        if (!raw) {  // Active low
          processStepButton(i, true);
        }
      }
    }
  }

  for (uint8_t i = 0; i < 4; ++i) {
    bool raw = readMuxChannel(MODE_BUTTON_CHANNELS[i]);
    ButtonState& state = mode_buttons[i];
    if (raw != state.last_reading) {
      state.last_change = now;
      state.last_reading = raw;
    }
    if ((now - state.last_change) >= BUTTON_DEBOUNCE_MS) {
      if (raw != state.stable_state) {
        state.stable_state = raw;
        if (!raw) {
          processModeButton(i, true);
        }
      }
    }
  }
}

void processStepButton(uint8_t index, bool pressed) {
  if (!pressed) {
    return;
  }
  portENTER_CRITICAL(&stepsMux);
  bool current = steps[index].active;
  steps[index].active = !current;
  portEXIT_CRITICAL(&stepsMux);
}

void processModeButton(uint8_t index, bool pressed) {
  if (!pressed) {
    return;
  }
  EditMode new_mode = static_cast<EditMode>(index);
  portENTER_CRITICAL(&stateMux);
  current_mode = new_mode;
  portEXIT_CRITICAL(&stateMux);
}

// -----------------------------------------------------------------------------
// Encoder handling (Core 1)
// -----------------------------------------------------------------------------
void readEncoders() {
  EditMode mode_snapshot;
  uint8_t current_step_snapshot;

  portENTER_CRITICAL(&stateMux);
  mode_snapshot = current_mode;
  current_step_snapshot = current_step;
  portEXIT_CRITICAL(&stateMux);

  const uint8_t encoder_index = static_cast<uint8_t>(mode_snapshot);
  if (encoder_index >= 4) {
    return;
  }

  long count = encoders[encoder_index].getCount();
  long last = encoder_last_count[encoder_index];
  long delta_counts = count - last;

  if (abs(delta_counts) >= ENCODER_TICKS_PER_DETENT) {
    encoder_last_count[encoder_index] = count;
    int32_t detents = delta_counts / ENCODER_TICKS_PER_DETENT;
    applyEncoderDelta(mode_snapshot, detents);
  }
}

void applyEncoderDelta(EditMode mode, int32_t delta) {
  if (delta == 0) {
    return;
  }

  uint8_t step_index;
  portENTER_CRITICAL(&stateMux);
  step_index = current_step;
  portEXIT_CRITICAL(&stateMux);

  StepData step;
  portENTER_CRITICAL(&stepsMux);
  step = readStep(steps[step_index]);
  portEXIT_CRITICAL(&stepsMux);

  switch (mode) {
    case MODE_NOTE:
      step.note = constrain(step.note + delta, 0, 127);
      break;
    case MODE_VELOCITY:
      step.velocity = constrain(step.velocity + delta, 1, 127);
      break;
    case MODE_DURATION: {
      int32_t duration = static_cast<int32_t>(step.duration_ms) + delta * 5;
      duration = constrain(duration, 20, 2000);
      step.duration_ms = static_cast<uint16_t>(duration);
      break;
    }
    case MODE_CHANNEL:
      step.channel = constrain(step.channel + delta, 1, 16);
      break;
  }

  portENTER_CRITICAL(&stepsMux);
  writeStep(steps[step_index], step);
  portEXIT_CRITICAL(&stepsMux);
}

// -----------------------------------------------------------------------------
// Visual feedback (Core 1)
// -----------------------------------------------------------------------------
void updateDisplay() {
  EditMode mode_snapshot;
  bool playing_snapshot;
  uint8_t current_step_snapshot;
  StepData step_snapshot;

  portENTER_CRITICAL(&stateMux);
  mode_snapshot = current_mode;
  playing_snapshot = is_playing;
  current_step_snapshot = current_step;
  portEXIT_CRITICAL(&stateMux);

  portENTER_CRITICAL(&stepsMux);
  step_snapshot = readStep(steps[current_step_snapshot]);
  portEXIT_CRITICAL(&stepsMux);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(playing_snapshot ? F("PLAYING ") : F("STOPPED "));
  display.print(modeToLabel(mode_snapshot));

  const int box_width = 14;
  const int box_height = 12;
  const int box_y = 16;

  for (uint8_t i = 0; i < NUM_STEPS; ++i) {
    int x = 4 + i * (box_width + 2);
    display.drawRect(x, box_y, box_width, box_height, SSD1306_WHITE);

    StepData s;
    portENTER_CRITICAL(&stepsMux);
  s = readStep(steps[i]);
    portEXIT_CRITICAL(&stepsMux);

    if (i == current_step_snapshot) {
      display.fillRect(x + 1, box_y + 1, box_width - 2, box_height - 2, SSD1306_WHITE);
    } else if (s.active) {
      display.fillRect(x + 3, box_y + 3, box_width - 6, box_height - 6, SSD1306_WHITE);
    }
  }

  display.setCursor(0, 40);
  display.print(F("Note: "));
  display.print(step_snapshot.note);
  display.setCursor(64, 40);
  display.print(F("Vel: "));
  display.print(step_snapshot.velocity);

  display.setCursor(0, 52);
  display.print(F("Dur: "));
  display.print(step_snapshot.duration_ms);
  display.print(F("ms"));

  display.setCursor(64, 52);
  display.print(F("Chan: "));
  display.print(step_snapshot.channel);

  display.display();
}

void updateLeds() {
  bool playing_snapshot;
  uint8_t current_step_snapshot;

  portENTER_CRITICAL(&stateMux);
  playing_snapshot = is_playing;
  current_step_snapshot = current_step;
  portEXIT_CRITICAL(&stateMux);

  for (uint8_t i = 0; i < NEOPIXEL_COUNT; ++i) {
    uint32_t color = 0;
    if (i < NUM_STEPS) {
      StepData s;
      portENTER_CRITICAL(&stepsMux);
      s = readStep(steps[i]);
      portEXIT_CRITICAL(&stepsMux);

      if (i == current_step_snapshot && playing_snapshot) {
        color = leds.Color(255, 0, 0);  // Active step in red
      } else if (s.active) {
        color = leds.Color(0, 0, 60);   // Enabled steps in blue
      }
    }
    leds.setPixelColor(i, color);
  }
  leds.show();
}

const __FlashStringHelper* modeToLabel(EditMode mode) {
  switch (mode) {
    case MODE_NOTE: return F("NOTE");
    case MODE_VELOCITY: return F("VEL");
    case MODE_DURATION: return F("DUR");
    case MODE_CHANNEL: return F("CHAN");
    default: return F("???");
  }
}

// -----------------------------------------------------------------------------
// FreeRTOS tasks
// -----------------------------------------------------------------------------
void midiTask(void* parameter) {
  for (;;) {
  #ifdef TINYUSB_NEED_POLLING_TASK
    TinyUSBDevice.task();
  #endif
    while (MIDI.read()) {
      // Realtime handlers execute via callbacks.
    }
    processPendingNoteOffs();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void uiTask(void* parameter) {
  TickType_t last_display_update = xTaskGetTickCount();
  TickType_t last_led_update = xTaskGetTickCount();

  for (;;) {
    scanButtons();
    readEncoders();

    if (xTaskGetTickCount() - last_display_update >= pdMS_TO_TICKS(40)) {
      updateDisplay();
      last_display_update = xTaskGetTickCount();
    }

    if (xTaskGetTickCount() - last_led_update >= pdMS_TO_TICKS(20)) {
      updateLeds();
      last_led_update = xTaskGetTickCount();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  initializeSteps();
  configureHardware();

  xTaskCreatePinnedToCore(midiTask, "MIDI Task", 4096, nullptr, configMAX_PRIORITIES - 1, &midiTaskHandle, 0);
  xTaskCreatePinnedToCore(uiTask, "UI Task",   8192, nullptr, 2, &uiTaskHandle, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));  // Idle; all work is done in tasks.
}