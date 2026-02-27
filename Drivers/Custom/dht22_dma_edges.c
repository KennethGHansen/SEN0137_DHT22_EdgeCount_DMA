#include "dht22_dma_edges.h"
#include "dwt_delay.h"
#include <string.h>

// ===== USER CONFIG =====
extern TIM_HandleTypeDef htim2;

#define DHT_PORT GPIOA
#define DHT_PIN  GPIO_PIN_1
#define DHT_AF   GPIO_AF1_TIM2

#define BUF_LEN  50          // need >= 41 pulse widths; 50 is safe
#define TIMEOUT_MS  25       // transfer lasts only a few ms; 25ms is generous
// =======================

typedef enum { S_IDLE=0, S_CAPTURING=1, S_READY=2 } state_t;
static volatile state_t st = S_IDLE;
static volatile uint32_t start_ms = 0;

// DMA buffers (HAL_TIM_IC_Start_DMA uses uint32_t*). [5](https://mikrocontroller.ti.bfh.ch/halDoc/group__TIM__Input__Capture__Polarity.html)[2](https://slideplayer.com/slide/17829299/)
static uint32_t rise[BUF_LEN];  // CCR2 captures rising edges on TI2 (CH2 pin)
static uint32_t fall[BUF_LEN];  // CCR1 captures falling edges on TI2 via indirect capture

static volatile uint16_t rise_count = 0;
static volatile uint16_t fall_count = 0;

// ---------- GPIO helpers ----------
static void DHT_SetOutputOD(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = DHT_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT_PORT, &g);
}

static void DHT_SetAF_Input(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = DHT_PIN;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = DHT_AF;
    HAL_GPIO_Init(DHT_PORT, &g);
}
// ----------------------------------

void DHT22_DMA_Init(void)
{
    DWT_Delay_Init();
}

void DHT22_DMA_Start(void)
{
    if (st != S_IDLE) return;

    memset(rise, 0, sizeof(rise));
    memset(fall, 0, sizeof(fall));
    rise_count = 0;
    fall_count = 0;

    // stop previous
    HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_2);
    HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_1);

    // Host start: low for >=1ms (use 2ms)
    DHT_SetOutputOD();
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
    HAL_Delay(2);

    // Release line & connect to timer
    DHT_SetAF_Input();
    delay_us(30);

    // Reset counter
    __HAL_TIM_SET_COUNTER(&htim2, 0);

    // --- Configure channels to capture TI2 (PA1) edges ---
    // CH2 direct TI2 rising
    __HAL_TIM_SET_CAPTUREPOLARITY(&htim2, TIM_CHANNEL_2, TIM_INPUTCHANNELPOLARITY_RISING);
    // CH1 indirect TI2 falling
    __HAL_TIM_SET_CAPTUREPOLARITY(&htim2, TIM_CHANNEL_1, TIM_INPUTCHANNELPOLARITY_FALLING);

    // IMPORTANT: CH1 must be configured as INDIRECTTI in CubeMX for TI2,
    // and CH2 as DIRECTTI. (Or configure in code if you do LL/register init.)

    st = S_CAPTURING;
    start_ms = HAL_GetTick();

    // --- Start DMA on CH2 then CH1 ---
    // HAL bug/limitation: starting DMA twice can return HAL_BUSY on F4;
    // workaround is to reset htim state between calls (documented by users). 

    if (HAL_TIM_IC_Start_DMA(&htim2, TIM_CHANNEL_2, (uint32_t*)rise, BUF_LEN) != HAL_OK) {
        st = S_IDLE;
        return;
    }

    // Workaround to allow starting second DMA channel
    htim2.State = HAL_TIM_STATE_READY;  // see discussion of HAL busy behavior 

    if (HAL_TIM_IC_Start_DMA(&htim2, TIM_CHANNEL_1, (uint32_t*)fall, BUF_LEN) != HAL_OK) {
        HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_2);
        st = S_IDLE;
        return;
    }
}

void DHT22_DMA_Service(void)
{
    if (st != S_CAPTURING) return;

    // Get how many samples have been written by DMA so far:
    // For F4, HAL exposes DMA counters via htim2.hdma[...] internally.
    // If CubeMX linked them, these pointers will be non-NULL.
    if (htim2.hdma[TIM_DMA_ID_CC2]) {
        uint16_t rem = __HAL_DMA_GET_COUNTER(htim2.hdma[TIM_DMA_ID_CC2]);
        rise_count = (uint16_t)(BUF_LEN - rem);
    }
    if (htim2.hdma[TIM_DMA_ID_CC1]) {
        uint16_t rem = __HAL_DMA_GET_COUNTER(htim2.hdma[TIM_DMA_ID_CC1]);
        fall_count = (uint16_t)(BUF_LEN - rem);
    }

    // Need enough pairs: at least 41 HIGH pulses = (ACK high + 40 bits)
    // A HIGH pulse requires one rise and one fall.
    uint16_t pairs = (rise_count < fall_count) ? rise_count : fall_count;
    if (pairs >= 41) {
        HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_2);
        HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_1);
        st = S_READY;
        return;
    }

    if ((HAL_GetTick() - start_ms) > TIMEOUT_MS) {
        HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_2);
        HAL_TIM_IC_Stop_DMA(&htim2, TIM_CHANNEL_1);
        st = S_IDLE;
    }
}

static DHT22_Status decode(float *tC, float *rh)
{
    uint16_t pairs = (rise_count < fall_count) ? rise_count : fall_count;
    if (pairs < 41) return DHT22_ERROR_FRAME;

    // Compute HIGH widths: high[i] = fall[i] - rise[i]
    // high[0] is ACK high (~80us). Bits are high[1..40]. [3](https://motherfasr376.weebly.com/stm32-hal-input-capture.html)
    uint8_t d[5] = {0};

    for (int b = 0; b < 40; b++) {
        uint32_t t_r = rise[b + 1];
        uint32_t t_f = fall[b + 1];
        uint32_t high_us = (uint32_t)(t_f - t_r);

        // DHT22: 0≈26–28us, 1≈70us; threshold around 50us. [3](https://motherfasr376.weebly.com/stm32-hal-input-capture.html)[6](https://deepbluembedded.com/stm32-gpio-write-pin-digital-output-lab/)
        uint8_t bit = (high_us > 50) ? 1 : 0;

        uint8_t byteIndex = b / 8;
        uint8_t bitPos = 7 - (b % 8);  // MSB first [4](https://blog.embeddedexpert.io/?p=2019)[3](https://motherfasr376.weebly.com/stm32-hal-input-capture.html)
        if (bit) d[byteIndex] |= (1U << bitPos);
    }

    // checksum = low 8-bit sum of first four bytes [4](https://blog.embeddedexpert.io/?p=2019)[3](https://motherfasr376.weebly.com/stm32-hal-input-capture.html)
    uint8_t sum = (uint8_t)(d[0] + d[1] + d[2] + d[3]);
    if (sum != d[4]) return DHT22_ERROR_CHECKSUM;

    uint16_t rawHum  = (uint16_t)((d[0] << 8) | d[1]);
    uint16_t rawTemp = (uint16_t)((d[2] << 8) | d[3]);

    int16_t signedTemp = (rawTemp & 0x8000) ? -(int16_t)(rawTemp & 0x7FFF) : (int16_t)rawTemp;

    *rh = rawHum * 0.1f;
    *tC = signedTemp * 0.1f;

    return DHT22_OK;
}

DHT22_Status DHT22_DMA_GetData(float *tC, float *rh)
{
    if (st == S_CAPTURING) return DHT22_BUSY;
    if (st == S_IDLE)      return DHT22_ERROR_TIMEOUT;

    // st == S_READY
    DHT22_Status r = decode(tC, rh);
    st = S_IDLE;
    return r;
}