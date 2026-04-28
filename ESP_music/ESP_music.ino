#include "Arduino.h"
#include "driver/i2s.h"
#include <math.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN       48
#define NUM_LEDS      1

#define I2S_BCLK_PIN  19
#define I2S_LRCK_PIN  20
#define I2S_DATA_PIN  21

#define TASTER_VOL_UP 35
#define TASTER_VOL_DN 36
#define TASTER_OCT_UP 37
#define TASTER_OCT_DN 38
#define SONG_CON_PIN  39
#define FORM_SWITCH   2

#define I2S_NUM       I2S_NUM_0
#define SAMPLE_RATE   16000
#define TABLE_SIZE    256
#define NUM_KEYS      14

#define ATTACK_RATE   0.1f
#define DECAY_RATE    0.9995f
#define SUSTAIN_LEVEL 0.8f
#define RELEASE_RATE  0.9990f

enum ADSRState { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };

const int touchPins[NUM_KEYS] = {T1, T2, T4, T6, T5, T7, T8, T3, T9, T10, T11, T12, T13, T14};
const int thresholds[NUM_KEYS] = {5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000, 5000};

float volume = 0.01;
int octshift = 0;

float basisNoten[NUM_KEYS] = {
  261.63, 277.18, 293.66, 311.13,
  329.63, 349.23, 369.99, 392.00,
  415.30, 440.00, 466.16, 493.88,
  523.25, 587.33
};

int16_t waveTable[TABLE_SIZE];
float phases[NUM_KEYS];
float amplitude[NUM_KEYS];
ADSRState adsrState[NUM_KEYS];

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void updateWaveTable() {
  bool isSaw = digitalRead(FORM_SWITCH) == HIGH;
  for (int i = 0; i < TABLE_SIZE; i++) {
    float v;
    if (isSaw) {
      v = (2.0 * i / TABLE_SIZE) - 1.0;
    } else {
      v = sin(2.0 * M_PI * i / TABLE_SIZE);
    }
    waveTable[i] = (int16_t)(v * 32767 * volume);
  }
}

void i2sInit() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_DATA_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM, &config, 0, NULL);
  i2s_set_pin(I2S_NUM, &pins);
  i2s_set_clk(I2S_NUM, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

void setup() {
  Serial.begin(115200);

  pinMode(TASTER_VOL_UP, INPUT_PULLDOWN);
  pinMode(TASTER_VOL_DN, INPUT_PULLDOWN);
  pinMode(TASTER_OCT_UP, INPUT_PULLDOWN);
  pinMode(TASTER_OCT_DN, INPUT_PULLDOWN);
  pinMode(SONG_CON_PIN,  INPUT_PULLDOWN);
  pinMode(FORM_SWITCH,   INPUT_PULLDOWN);

  for (int i = 0; i < NUM_KEYS; i++) {
    phases[i]    = 0;
    amplitude[i] = 0;
    adsrState[i] = IDLE;
  }

  updateWaveTable();
  i2sInit();
  strip.begin();
  strip.show();
}

void loop() {
  static int16_t buffer[512 * 2];
  size_t bytesWritten;
  bool keyPressed[NUM_KEYS] = {false};
  int activeKeys = 0;

  for (int k = 0; k < NUM_KEYS; k++) {
    if ((touchRead(touchPins[k]) / 10) > thresholds[k]) {
      keyPressed[k] = true;
      activeKeys++;
      if (adsrState[k] == IDLE || adsrState[k] == RELEASE) {
        adsrState[k] = ATTACK;
      }
    } else {
      if (adsrState[k] == ATTACK || adsrState[k] == DECAY || adsrState[k] == SUSTAIN) {
        adsrState[k] = RELEASE;
      }
    }
  }

  if (digitalRead(TASTER_VOL_UP) == HIGH) {
    volume = min(1.0f, volume + 0.005f);
    updateWaveTable();
  }
  if (digitalRead(TASTER_VOL_DN) == HIGH) {
    volume = max(0.0f, volume - 0.005f);
    updateWaveTable();
  }
  if (digitalRead(TASTER_OCT_UP) == HIGH) {
    octshift = min(2, octshift + 1);
    strip.setPixelColor(0, strip.Color(0, 155, 0));
    strip.show();
    delay(200);
  }
  if (digitalRead(TASTER_OCT_DN) == HIGH) {
    octshift = max(-2, octshift - 1);
    strip.setPixelColor(0, strip.Color(155, 0, 0));
    strip.show();
    delay(200);
  }

  float octFactor = pow(2, octshift);

  for (int i = 0; i < 512; i++) {
    int32_t mixedSample = 0;
    float ampSum = 0;

    for (int k = 0; k < NUM_KEYS; k++) {
      switch (adsrState[k]) {
        case ATTACK:
          amplitude[k] += ATTACK_RATE;
          if (amplitude[k] >= 1.0f) {
            amplitude[k] = 1.0f;
            adsrState[k] = DECAY;
          }
          break;

        case DECAY:
          amplitude[k] *= DECAY_RATE;
          if (amplitude[k] <= SUSTAIN_LEVEL) {
            amplitude[k] = SUSTAIN_LEVEL;
            adsrState[k] = SUSTAIN;
          }
          break;

        case SUSTAIN:
          amplitude[k] = SUSTAIN_LEVEL;
          break;

        case RELEASE:
          amplitude[k] *= RELEASE_RATE;
          if (amplitude[k] < 0.001f) {
            amplitude[k] = 0;
            adsrState[k] = IDLE;
          }
          break;

        case IDLE:
          break;
      }

      if (adsrState[k] != IDLE) {
        float step = (basisNoten[k] * octFactor) * TABLE_SIZE / SAMPLE_RATE;
        phases[k] += step;
        if (phases[k] >= TABLE_SIZE) phases[k] -= TABLE_SIZE;

        mixedSample += waveTable[(int)phases[k]] * amplitude[k];
        ampSum += amplitude[k];
      }
    }

    if (ampSum > 1.0f) {
      mixedSample /= (ampSum * 0.8f);
    }

    int16_t finalSample = (int16_t)constrain(mixedSample, -32767, 32767);
    buffer[i * 2]     = finalSample;
    buffer[i * 2 + 1] = finalSample;
  }

  i2s_write(I2S_NUM, buffer, sizeof(buffer), &bytesWritten, portMAX_DELAY);

  if (activeKeys > 0) {
    strip.setPixelColor(0, strip.Color(0, 155, 0));
  } else {
    strip.setPixelColor(0, strip.Color(0, 0, 155));
  }
  strip.show();
}