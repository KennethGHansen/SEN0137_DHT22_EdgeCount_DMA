#ifndef DHT22_DMA_EDGES_H
#define DHT22_DMA_EDGES_H

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_tim.h"

typedef enum {
    DHT22_OK = 0,
    DHT22_BUSY,
    DHT22_ERROR_TIMEOUT,
    DHT22_ERROR_FRAME,
    DHT22_ERROR_CHECKSUM
} DHT22_Status;

void DHT22_DMA_Init(void);
void DHT22_DMA_Start(void);
void DHT22_DMA_Service(void);
DHT22_Status DHT22_DMA_GetData(float *tC, float *rh);

#endif
