#include "audio_cd_hal.h"

#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <stm32wbxx_ll_dma.h>
#include <stm32wbxx_ll_gpio.h>
#include <stm32wbxx_ll_tim.h>

#define AUDIO_CD_SPEAKER_TIMER   TIM16
#define AUDIO_CD_SAMPLE_TIMER    TIM2
#define AUDIO_CD_SPEAKER_CHANNEL LL_TIM_CHANNEL_CH1
#define AUDIO_CD_DMA_INSTANCE    DMA1, LL_DMA_CHANNEL_1

void audio_cd_hal_init(uint32_t sample_rate, bool speaker_output, bool external_output) {
    furi_hal_bus_enable(FuriHalBusTIM2);

    LL_TIM_InitTypeDef timer = {0};
    timer.Prescaler = 1;
    timer.Autoreload = UINT8_MAX;
    LL_TIM_Init(AUDIO_CD_SPEAKER_TIMER, &timer);

    LL_TIM_OC_InitTypeDef output = {0};
    output.OCMode = LL_TIM_OCMODE_PWM1;
    output.OCState = LL_TIM_OCSTATE_ENABLE;
    output.CompareValue = UINT8_MAX / 2;
    LL_TIM_OC_Init(AUDIO_CD_SPEAKER_TIMER, AUDIO_CD_SPEAKER_CHANNEL, &output);

    timer.Prescaler = 0;
    timer.Autoreload = SystemCoreClock / sample_rate - 1;
    LL_TIM_Init(AUDIO_CD_SAMPLE_TIMER, &timer);

    output.CompareValue = 1;
    LL_TIM_OC_Init(AUDIO_CD_SAMPLE_TIMER, AUDIO_CD_SPEAKER_CHANNEL, &output);

    if(speaker_output) {
        furi_hal_gpio_init_ex(
            &gpio_speaker, GpioModeAltFunctionPushPull, GpioPullNo, GpioSpeedLow, GpioAltFn14TIM16);
    } else {
        furi_hal_gpio_init(&gpio_speaker, GpioModeAnalog, GpioPullDown, GpioSpeedLow);
    }

    if(external_output) {
        furi_hal_gpio_init_ex(
            &gpio_ext_pa6,
            GpioModeAltFunctionPushPull,
            GpioPullNo,
            GpioSpeedVeryHigh,
            GpioAltFn14TIM16);
    } else {
        furi_hal_gpio_init(&gpio_ext_pa6, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }
}

void audio_cd_hal_deinit(void) {
    furi_hal_gpio_init(&gpio_ext_pa6, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_bus_disable(FuriHalBusTIM2);
}

void audio_cd_hal_speaker_start(void) {
    LL_TIM_EnableAllOutputs(AUDIO_CD_SPEAKER_TIMER);
    LL_TIM_EnableCounter(AUDIO_CD_SPEAKER_TIMER);
    LL_TIM_EnableAllOutputs(AUDIO_CD_SAMPLE_TIMER);
    LL_TIM_EnableCounter(AUDIO_CD_SAMPLE_TIMER);
}

void audio_cd_hal_speaker_stop(void) {
    LL_TIM_DisableAllOutputs(AUDIO_CD_SAMPLE_TIMER);
    LL_TIM_DisableCounter(AUDIO_CD_SAMPLE_TIMER);
    LL_TIM_DisableAllOutputs(AUDIO_CD_SPEAKER_TIMER);
    LL_TIM_DisableCounter(AUDIO_CD_SPEAKER_TIMER);
}

void audio_cd_hal_dma_init(uint32_t address, size_t size) {
    uint32_t destination = (uint32_t)&AUDIO_CD_SPEAKER_TIMER->CCR1;

    LL_DMA_ConfigAddresses(
        AUDIO_CD_DMA_INSTANCE, address, destination, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetDataLength(AUDIO_CD_DMA_INSTANCE, size);
    LL_DMA_SetPeriphRequest(AUDIO_CD_DMA_INSTANCE, LL_DMAMUX_REQ_TIM2_UP);
    LL_DMA_SetDataTransferDirection(AUDIO_CD_DMA_INSTANCE, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetChannelPriorityLevel(AUDIO_CD_DMA_INSTANCE, LL_DMA_PRIORITY_VERYHIGH);
    LL_DMA_SetMode(AUDIO_CD_DMA_INSTANCE, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetPeriphIncMode(AUDIO_CD_DMA_INSTANCE, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(AUDIO_CD_DMA_INSTANCE, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(AUDIO_CD_DMA_INSTANCE, LL_DMA_PDATAALIGN_HALFWORD);
    LL_DMA_SetMemorySize(AUDIO_CD_DMA_INSTANCE, LL_DMA_MDATAALIGN_BYTE);
    LL_DMA_EnableIT_TC(AUDIO_CD_DMA_INSTANCE);
    LL_DMA_EnableIT_HT(AUDIO_CD_DMA_INSTANCE);
}

void audio_cd_hal_dma_start(void) {
    LL_DMA_ClearFlag_HT1(DMA1);
    LL_DMA_ClearFlag_TC1(DMA1);
    LL_DMA_EnableChannel(AUDIO_CD_DMA_INSTANCE);
    LL_TIM_EnableDMAReq_UPDATE(AUDIO_CD_SAMPLE_TIMER);
}

void audio_cd_hal_dma_stop(void) {
    LL_TIM_DisableDMAReq_UPDATE(AUDIO_CD_SAMPLE_TIMER);
    LL_DMA_DisableChannel(AUDIO_CD_DMA_INSTANCE);
    LL_DMA_DisableIT_HT(AUDIO_CD_DMA_INSTANCE);
    LL_DMA_DisableIT_TC(AUDIO_CD_DMA_INSTANCE);
    LL_DMA_ClearFlag_HT1(DMA1);
    LL_DMA_ClearFlag_TC1(DMA1);
}
