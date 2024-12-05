#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <math.h>
#include <pthread.h>
#include "physical.h"
#include "audio.h"

// Define the mutex
pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread function for playing the coin sound
void* play_audio_thread(void* arg) {
    void* audio_virtual_base = arg;

    // Lock the mutex to ensure exclusive access to audio playback
    pthread_mutex_lock(&audio_mutex);

    // Play the coin sound
    int i;
    double freq1_rad = COIN_FREQ1 * PI2 / SAMPLING_RATE;
    double freq2_rad = COIN_FREQ2 * PI2 / SAMPLING_RATE;
    int samples_per_tone = (SAMPLING_RATE * COIN_DURATION) / 1000;

    // Play first tone (B5)
    for (i = 0; i < samples_per_tone; i++) {
        int sample = (int)(MAX_VOLUME / 4 * sin(i * freq1_rad));
        write_audio_sample(audio_virtual_base, sample);
    }

    // Play second tone (E6)
    for (i = 0; i < samples_per_tone; i++) {
        int sample = (int)(MAX_VOLUME / 4 * sin(i * freq2_rad));
        write_audio_sample(audio_virtual_base, sample);
    }

    // Unlock the mutex after playback
    pthread_mutex_unlock(&audio_mutex);

    return NULL;
}

void play_game_over_sound(void* audio_virtual_base) {
    pthread_mutex_lock(&audio_mutex);

    const double notes[] = {391.995, 349.228, 329.628}; // G, F, E frequencies
    const int note_durations[] = {1000, 1000, 2000};    // Durations in ms (4 seconds total)
    const int num_notes = sizeof(notes) / sizeof(notes[0]);

    for (int i = 0; i < num_notes; i++) {
        double frequency_rad = notes[i] * PI2 / SAMPLING_RATE;
        int num_samples = (SAMPLING_RATE * note_durations[i]) / 1000;

        for (int j = 0; j < num_samples; j++) {
            int sample = (int)(MAX_VOLUME / 4 * sin(j * frequency_rad));
            write_audio_sample(audio_virtual_base, sample);
        }
    }

    pthread_mutex_unlock(&audio_mutex);
}

// Write a single audio sample to both left and right channels
int write_audio_sample(void* audio_virtual_base, int sample) {
    while ((*((volatile int*)(audio_virtual_base + FIFOSPACE_REG)) & 0x00FF0000) == 0);
    *((volatile int*)(audio_virtual_base + LEFTDATA_REG)) = sample;
    *((volatile int*)(audio_virtual_base + RIGHTDATA_REG)) = sample;
    return 0;
}

// Clear the audio FIFOs
void clear_audio_fifos(void* audio_virtual_base) {
    *((volatile int*)(audio_virtual_base + CONTROL_REG)) = 0x4;
    *((volatile int*)(audio_virtual_base + CONTROL_REG)) = 0x0;
}

// Wait until audio FIFO is empty
void wait_audio_fifo_empty(void* audio_virtual_base) {
    while ((*((volatile int*)(audio_virtual_base + FIFOSPACE_REG)) & 0x00FF0000) != 0x00FF0000);
}

// Helper function to start the coin sound thread
void start_coin_sound(void* audio_virtual_base) {
    pthread_t audio_thread;
    clear_audio_fifos(audio_virtual_base);

    // Create a new thread to play the coin sound
    if (pthread_create(&audio_thread, NULL, play_audio_thread, audio_virtual_base) != 0) {
        perror("Failed to create audio thread");
        return;
    }

    // Detach the thread to let it clean up automatically
    pthread_detach(audio_thread);
}
