#pragma once
#include "esp_err.h"

// Sets up I2S for the MAX98357A and starts the audio task.
esp_err_t audio_init(void);

// Plays the chime in the background. Returns immediately.
// Calling it again while a chime is playing doesn't stack up extra chimes.
void audio_chime(void);