#ifndef AUDIO_H_
#define AUDIO_H_

#include <math.h>
#include <pthread.h>

extern pthread_mutex_t audio_mutex;

// Audio constants
#define PI 3.14159265
#define PI2 6.28318531
#define SAMPLING_RATE 8000
#define MAX_VOLUME 0x7fffffff
#define SAMPLE_DURATION 300    // Duration in milliseconds

// Audio Core Registers
#define AUDIO_BASE 0xFF203040
#define AUDIO_SPAN 16
#define CONTROL_REG 0x0
#define FIFOSPACE_REG 0x4
#define LEFTDATA_REG 0x8
#define RIGHTDATA_REG 0xC

// For Super Mario coin sound
#define COIN_FREQ1 988.0   // B5
#define COIN_FREQ2 1319.0  // E6
#define COIN_DURATION 50   // 50ms per tone

// Function prototypes
int write_audio_sample(void* audio_virtual_base, int sample);
void clear_audio_fifos(void* audio_virtual_base);
void wait_audio_fifo_empty(void* audio_virtual_base);
void play_coin_sound(void* audio_virtual_base);
void* play_audio_thread(void* arg);
void start_coin_sound(void* audio_virtual_base);
void play_game_over_sound(void* audio_virtual_base);

#endif /* AUDIO_H_ */
