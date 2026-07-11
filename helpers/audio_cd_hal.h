#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void audio_cd_hal_init(uint32_t sample_rate, bool speaker_output, bool external_output);
void audio_cd_hal_deinit(void);
void audio_cd_hal_speaker_start(void);
void audio_cd_hal_speaker_stop(void);
void audio_cd_hal_dma_init(uint32_t address, size_t size);
void audio_cd_hal_dma_start(void);
void audio_cd_hal_dma_stop(void);
