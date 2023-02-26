/*

  AcidBox
  ESP32 headless acid combo of 303 + 303 + 808 like synths. MIDI driven. I2S output. No indication. Uses both cores of ESP32.

  To build the thing
  You will need an ESP32 with PSRAM (ESP32 WROVER module). Preferrable an external DAC, like PCM5102. In ArduinoIDE Tools menu select:

* * Board: ESP32 Dev Module
* * Partition scheme: No OTA (1MB APP/ 3MB SPIFFS)
* * PSRAM: enabled

  Also you will need to upload samples from /data folder to the ESP32 flash. To do so follow the instructions:
  https://github.com/lorol/LITTLEFS#arduino-esp32-littlefs-filesystem-upload-tool
  And then use Tools -> ESP32 Sketch Data Upload

*/

#include "config.h"

#include "driver/i2s.h"
#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"
#include "synthvoice.h"
#include "sampler.h"
#include <Wire.h>

#if defined MIDI_VIA_SERIAL2 || defined MIDI_VIA_SERIAL
#include <MIDI.h>
#endif

#ifdef MIDI_VIA_SERIAL
// default settings for Hairless midi is 115200 8-N-1
struct CustomBaudRateSettings : public MIDI_NAMESPACE::DefaultSerialSettings {
  static const long BaudRate = 115200;
};
MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings> serialMIDI(Serial);
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings>> MIDI((MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings>&)serialMIDI);
#endif

#ifdef MIDI_VIA_SERIAL2
// MIDI port on UART2,   pins 16 (RX) and 17 (TX) prohibited, as they are used for PSRAM
struct Serial2MIDISettings : public midi::DefaultSettings {
  static const long BaudRate = 31250;
  static const int8_t RxPin  = MIDIRX_PIN;
  static const int8_t TxPin  = MIDITX_PIN;
};
MIDI_NAMESPACE::SerialMIDI<HardwareSerial> Serial2MIDI2(Serial2);
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>> MIDI2((MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>&)Serial2MIDI2);
#endif

const i2s_port_t i2s_num = I2S_NUM_0; // i2s port number

// lookuptables
static float midi_pitches[128];
static float midi_phase_steps[128];
static float midi_tbl_steps[128];
static float exp_square_tbl[WAVE_SIZE];
//static float square_tbl[WAVE_SIZE];
//static float saw_tbl[WAVE_SIZE];
static float exp_tbl[WAVE_SIZE];
static float tanh_tbl[WAVE_SIZE];
static uint32_t last_reset = 0;
static float param[POT_NUM] ;
//static float (*tables[])[WAVE_SIZE] = {&exp_square_tbl, &square_tbl, &saw_tbl, &exp_tbl};

// Audio buffers of all kinds
static float synth_buf[2][DMA_BUF_LEN]; // 2 * 303 mono
static float drums_buf_l[DMA_BUF_LEN];  // 808 stereo L
static float drums_buf_r[DMA_BUF_LEN];  // 808 stereo R
static float mix_buf_l[DMA_BUF_LEN];    // mix L channel
static float mix_buf_r[DMA_BUF_LEN];    // mix R channel
static union { // a dirty trick, instead of true converting
  int16_t _signed[DMA_BUF_LEN * 2];
  uint16_t _unsigned[DMA_BUF_LEN * 2];
} out_buf; // i2s L+R output buffer

volatile boolean processing = false;
#ifndef NO_PSRAM
volatile float rvb_k1, rvb_k2, rvb_k3;
#endif
volatile float dly_k1, dly_k2, dly_k3;

size_t bytes_written; // i2s
volatile uint32_t s1t, s2t, drt, fxt, s1T, s2T, drT, fxT, art, arT; // debug timing: if we use less vars, compiler optimizes them
volatile static uint32_t prescaler;

// tasks for Core0 and Core1
TaskHandle_t SynthTask1;
TaskHandle_t SynthTask2;

// 303-like synths
SynthVoice Synth1(0); // use synth_buf[0]
SynthVoice Synth2(1); // use synth_buf[1]

// 808-like drums
Sampler Drums(DRUMKITCNT , DEFAULT_DRUMKIT); // first arg = total number of sample sets, second = starting drumset [0 .. total-1]

// Global effects
FxDelay Delay;
#ifndef NO_PSRAM
FxReverb Reverb;
#endif
Compressor Comp;

hw_timer_t * timer1 = NULL;            // Timer variables
hw_timer_t * timer2 = NULL;            // Timer variables
portMUX_TYPE timer1Mux = portMUX_INITIALIZER_UNLOCKED; 
portMUX_TYPE timer2Mux = portMUX_INITIALIZER_UNLOCKED; 
volatile boolean timer1_fired = false;   // Update battery icon flag
volatile boolean timer2_fired = false;   // Update battery icon flag

/*
 * Timer interrupt handler **********************************************************************************************************************************
*/

void IRAM_ATTR onTimer1() {
   portENTER_CRITICAL_ISR(&timer1Mux);
   timer1_fired = true;
   portEXIT_CRITICAL_ISR(&timer1Mux);
}

void IRAM_ATTR onTimer2() {
   portENTER_CRITICAL_ISR(&timer2Mux);
   timer2_fired = true;
   portEXIT_CRITICAL_ISR(&timer2Mux);
}


/* 
 *  Quite an ordinary SETUP() *******************************************************************************************************************************
*/

void setup(void) {

  btStop();

#ifdef MIDI_VIA_SERIAL
  Serial.begin(115200, SERIAL_8N1);
#endif
#ifdef MIDI_VIA_SERIAL2
  pinMode( MIDIRX_PIN , INPUT_PULLDOWN);
  pinMode( MIDITX_PIN , OUTPUT);
  Serial2.begin( 31250, SERIAL_8N1, MIDIRX_PIN, MIDITX_PIN ); // midi port
#endif

#ifdef DEBUG_ON
#ifndef MIDI_VIA_SERIAL
  Serial.begin(115200);
#endif
#endif
  /*
    for (uint8_t i = 0; i < GPIO_BUTTONS; i++) {
    pinMode(buttonGPIOs[i], INPUT_PULLDOWN);
    }
  */

  buildTables();

  for (int i = 0; i < POT_NUM; i++) pinMode( POT_PINS[i] , INPUT_PULLDOWN);


#ifdef MIDI_VIA_SERIAL
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleCC);
  MIDI.setHandleProgramChange(handleProgramChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.setHandleNoteOn(handleNoteOn);
  MIDI2.setHandleNoteOff(handleNoteOff);
  MIDI2.setHandleControlChange(handleCC);
  MIDI2.setHandleProgramChange(handleProgramChange);
  MIDI2.begin(MIDI_CHANNEL_OMNI);
#endif

  Synth1.Init();
  Synth2.Init();
  Drums.Init();
#ifndef NO_PSRAM
  Reverb.Init();
#endif
  Delay.Init();
  Comp.Init(SAMPLE_RATE);
#ifdef JUKEBOX
  init_midi();
#endif

  // silence while we haven't loaded anything reasonable
  for (int i = 0; i < DMA_BUF_LEN; i++) {
    drums_buf_l[i] = 0.0f ;
    drums_buf_r[i] = 0.0f ;
    synth_buf[0][i] = 0.0f ;
    synth_buf[1][i] = 0.0f ;
    out_buf._signed[i * 2] = 0 ;
    out_buf._signed[i * 2 + 1] = 0 ;
    mix_buf_l[i] = 0.0f;
    mix_buf_r[i] = 0.0f;
  }

  i2sInit();
  i2s_write(i2s_num, out_buf._signed, sizeof(out_buf._signed), &bytes_written, portMAX_DELAY);

  //xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask1, 0 );
  //xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask2, 1 );
 xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 8000, NULL, 2, &SynthTask1, 0 );
 xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 8000, NULL, 2, &SynthTask2, 1 );

  // somehow we should allow tasks to run
  xTaskNotifyGive(SynthTask1);
  xTaskNotifyGive(SynthTask2);
  processing = true;

  // timer interrupt
  /*
  timer1 = timerBegin(0, 80, true);               // Setup timer for midi
  timerAttachInterrupt(timer1, &onTimer1, true);  // Attach callback
  timerAlarmWrite(timer1, 4000, true);            // 4000us, autoreload
  timerAlarmEnable(timer1);
*/
  timer2 = timerBegin(1, 80, true);               // Setup general purpose timer
  timerAttachInterrupt(timer2, &onTimer2, true);  // Attach callback
  timerAlarmWrite(timer2, 200000, true);          // 200ms, autoreload
  timerAlarmEnable(timer2);
}

static uint32_t last_ms = micros();

/* 
 *  Finally, the LOOP () ***********************************************************************************************************
*/

void loop() { // default loopTask running on the Core1
  // you can still place some of your code here
  // or   vTaskDelete(NULL);
  
  // processButtons();

  loop_250Hz();
    
  taskYIELD(); // this can wait
}

/* 
 * Core Tasks ************************************************************************************************************************
*/

// Core0 task
static void audio_task1(void *userData) {
  while (true) {
    // we can run it together with synth(), but not with mixer()
    drt = micros();
    drums_generate();
    drT = micros() - drt;
    taskYIELD();
    s2t = micros();
    Synth2.Generate();
    s2T = micros() - s2t;
    taskYIELD();
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) { // we need all the generators to fill the buffers here, so we wait
      fxt = micros();
      mixer(); // actually we could send Notify before mixer() is done, but then we'd need tic-tac buffers for generation. Todo maybe
      fxT = micros() - fxt;
      xTaskNotifyGive(SynthTask2);
    }

    i2s_output();

    taskYIELD();
  }
}

// task for Core1, which tipically runs user's code on ESP32
static void audio_task2(void *userData) {
  while (true) {
    taskYIELD();
    // this part of the code never intersects with mixer buffers
    // this part of the code is operating with shared resources, so we should make it safe
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
      s1t = micros();
      Synth1.Generate();
      s1T = micros() - s1t;
      xTaskNotifyGive(SynthTask1); // if you have glitches, you may want to place this string in the end of audio_task2
    }
    
    taskYIELD();
    if (timer2_fired) {
      timer2_fired = false;
      art = micros();
      //readPots();
      arT = micros() - art;
#ifdef DEBUG_TIMING
    // DEBF ("synt1=%dus synt2=%dus drums=%dus mixer=%dus DMA_BUF=%dus\r\n" , s1T, s2T, drT, fxT, DMA_BUF_TIME);
    DEBF ("Core0=%dus Core1=%dus DMA_BUF=%dus AnalogRead=%dus\r\n" , s2T + drT + fxT, s1T, DMA_BUF_TIME, arT);
#endif
    }    
    
    taskYIELD();
    // hopefully, other Core1 tasks (for example, loop()) run here
  }
}

/* 
 *  Some debug and service routines *****************************************************************************************************************************
*/




void readPots() {
  static const float snap = 0.008f;
  static uint8_t i = 0;
  static float tmp;
  static const float NORMALIZE_ADC = 1.0f / 4096.0f;
//read one pot per call
  tmp = (float)analogRead(POT_PINS[i]) * NORMALIZE_ADC;
  if (fabs(tmp - param[i]) > snap) {
    param[i] = tmp;
    paramChange(i, tmp);
  }

  i++;
  if (i >= POT_NUM) i=0;
}

void paramChange(uint8_t paramNum, float paramVal) {
  // paramVal === param[paramNum];

 // DEBF ("param %d val %0.4f\r\n" , paramNum, paramVal);
  switch (paramNum) {
    case 0:
      //set_bpm( 40.0f + (paramVal * 160.0f));
      Synth1.SetCutoff(paramVal);
      break;
    case 1:
      Synth1.SetReso(paramVal);
      break;
    case 2:
      Synth1.SetOverdriveLevel(paramVal);
      break;
    default:
      {}
  }
}


#ifdef JUKEBOX
void jukebox_tick() {
  run_tick();
  myRandomAddEntropy((uint16_t)(micros() & 0x0000FFFF));
}
#endif


void loop_250Hz() {
  timer1_fired = false;
  
#ifdef MIDI_VIA_SERIAL
  MIDI.read();
#endif

#ifdef MIDI_VIA_SERIAL2
  MIDI2.read();
#endif
  
#ifdef JUKEBOX
  jukebox_tick();
#endif



}
