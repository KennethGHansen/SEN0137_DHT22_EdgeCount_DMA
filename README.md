# **SEN0137 DHT22 (DMA‑Based Dual‑Channel Capture)**

High-performance STM32 HAL driver for the **SEN0137 / DHT22 / AM2302** sensor using **TIM input capture + DMA on two channels**.

This version eliminates per-edge CPU interrupts and is suitable for **low-power**, **high-load**, or **RTOS-based** systems.

---

## Features

- **DMA-based** edge capture (near-zero CPU overhead)
- Separate capture of **rising and falling edges**
- No ambiguity about edge polarity
- Robust decoding of DHT22 pulse-width protocol
- Checksum-validated results
- Deterministic timing, production-grade design

---

## Hardware

| Component | Description |
|---------|------------|
| Sensor | SEN0137 / DHT22 / AM2302 |
| MCU | STM32F446RE (tested) |
| Timer | TIM2 |
| GPIO | PA1 (TI2 input) |
| Pull-up | 4.7kΩ–10kΩ |

- NOTE: Supply the SEN0137 with +5V and pull up with 4k7 to 3V3 for smooth functionality

---

##  Architecture Overview

This driver uses **two timer channels connected to the same pin**:

| Channel | Mode | Edge | Purpose |
|-------|------|------|--------|
| TIM2_CH2 | Direct TI2 | Rising | Capture start of HIGH pulses |
| TIM2_CH1 | Indirect TI2 | Falling | Capture end of HIGH pulses |

Each channel uses DMA to store timestamps directly into memory.

HIGH pulse width is computed as:
HIGH_us = fall[i] - rise[i]

This avoids all guesswork and end-of-frame issues.

---

## Timer Configuration (CubeMX)

- Timer: **TIM2**
- Channel: **CH2**
- Trigger Source: ITR0
- Clock Source: Internal clock
- Mode CH1: Input Capture Indirect
- Mode CH2: Input Capture Direct
- Polarity CH1: **Falling**
- Polarity CH2: **Rising**
- Prescaler: Set for **1 µs tick** (Prescaler = 89, Period = 65535)
- Counter mode: Up
- TIMER DMA CH2: Word (32 Bit)
- TIMER DMA CH1: Word (32 Bit)
- Direction: Peripheral → Memory
- Mode: Normal
- NVIC: TIM2 interrupt enabled (Enable DMA stream IRQs used by TIM2)

---

## Sys Clock Configuration (CubeMX)
HSI -> PLLM: 8 -> PLLN: x180 -> PLLP: /2 -> 
PLLCLK -> HCLK: 180 -> APB1: /4 -> APB2: -> /2

---  

📌 Notes on HAL

HAL has a known limitation when starting DMA on two TIM channels
This driver safely resets the TIM state between DMA starts
Tested specifically on STM32F446RE

---

## Project Structure
```
SEN0137_DHT22_EdgeCount_DMA/
├── Core/
│   ├── Inc/
│   └── Src/
│
├── Drivers/
│   └── Custom/
│       ├── dht22_dma_edges.c
│       ├── dht22_dma_edges.h
│       └── dwt_delay.h
│
├── MDK-ARM/
│   ├── SEN0137_DHT22_EdgeCount_DMA.uvprojx
│   ├── SEN0137_DHT22_EdgeCount_DMA.uvoptx
│
├── README.md
├── LICENSE
├── .gitignore
└──SEN0137_DHT22_EdgeCount_DMA.ioc
```

---   

Keil users: be sure to add  
`Drivers/Custom/`  
to the **include paths in uvision**

##  Getting Started

### 1. Clone the repository

### 2. Open the Keil `.uvprojx` project file

### 3. Make sure the include paths are correct
Keil →  
**Project → Options for Target → C/C++ → Include Paths**
Add:
../Drivers/Custom

### 4. Build and flash
Press **Build** and **Download** in Keil.

---

##  Usage Example

```c
float temperature, humidity;
DHT22_Status status;

DHT22_DMA_Start();

uint32_t t0 = HAL_GetTick();
do {
    DHT22_DMA_Service();
    status = DHT22_DMA2CH_GetData(&temperature, &humidity);
} while (status == DHT22_BUSY && (HAL_GetTick() - t0) < 100);

if (status == DHT22_OK) {
    printf("T=%.1f C  RH=%.1f %%\n", temperature, humidity);
}

```
---

## Why this version is superior to only using interrupt, not DMA
| Aspect | Interrupt Version | DMA Version |
|-------|------|------|
| CPU Load | Medium | Very low |
| Edge Polarity | Software Tracked | Hardware-guaranteed |
| Last‑bit ambiguity | Needs care | Impossible |
| RTOS Friendly | Good | Exellent |
| Reliability | High | Very High |

---

## Flow Chart
<img width="1100" height="1500" alt="default_image_001" src="https://github.com/user-attachments/assets/7db53cc3-6e06-419b-a310-36d399073312" />

---

## Driver flowchart detailed description
1️⃣ Start
The application decides to read the DHT22.

2️⃣ DHT22_DMA_Start()
Initializes a single DHT22 transaction.
Actions:
- Clear DMA buffers
- Reset counters
- Stop any previous TIM/DMA activity

3️⃣ MCU pulls DATA LOW (≈2 ms)
DHT22 start signal:
- Host pulls bus LOW ≥1 ms (2 ms used for margin)
- Sensor detects this as a request

4️⃣ MCU releases line → AF TIM2 input
GPIO switches from open‑drain output to alternate function
MCU releases control of the line
DHT22 now drives the bus

5️⃣ TIM2 configured for DMA input capture
Timer setup:
- Timer runs at 1 µs resolution
- Two capture channels are enabled
- DMA is armed for both channels

6️⃣ CH2: Rising edges → DMA → rise[]
TIM2 Channel 2 (Direct TI2)
Captures rising edges
DMA stores timestamps into rise[]

7️⃣ CH1: Falling edges → DMA → fall[]
TIM2 Channel 1 (Indirect TI2)
Captures falling edges
DMA stores timestamps into fall[]

✅ This preserves edge polarity, which is why the DMA solution is reliable.

8️⃣ DHT22_DMA_Service()
Runs repeatedly while BUSY.
Responsibilities:
- Monitor DMA progress
- Count how many rise/fall samples are available
- Enforce a safety timeout

9️⃣ Enough rise/fall pairs (≥41)?
Decision point:
1 pair = 1 HIGH pulse
Need:
1 × ACK HIGH
40 × data HIGH pulses
If yes → stop capture
If no → keep waiting (or timeout)

🔟 Stop DMA + Timer
DMA transfers are halted
Timer capture is stopped
System transitions to “decode” phase

1️⃣1️⃣ Compute HIGH widths = fall[i] − rise[i]
Pulse decoding:
- Each HIGH pulse width is calculated directly
- No guessing, no tail‑timing hacks

1️⃣2️⃣ Skip ACK HIGH pulse (~80 µs)
First HIGH pulse is the sensor ACK
Data bits start after this

1️⃣3️⃣ Decode 40 bits (threshold ≈50 µs)
For each HIGH pulse:
~26–28 µs → bit 0
~70 µs → bit 1
Threshold ~50 µs cleanly separates the two.

1️⃣4️⃣ Pack 5 bytes MSB‑first
Bits are assembled exactly as specified:
[ HumH | HumL | TempH | TempL | Checksum ]

❌ No → frame rejected
✅ Yes → data is valid

1️⃣6️⃣ Return Temperature & Humidity
Final conversion:
- Humidity = raw / 10
- Temperature = signed raw / 10
Values are now stable, accurate, and repeatable.

---

## Tested On

STM32F446RE @ 180 MHz
HAL drivers
DHT22 module and bare sensor

---

## License
This project is released under the MIT License.
You are free to use, modify, and share.

---

## Contributions
Pull requests and improvements are welcome.

---

## Acknowledgements
Thanks to the ST community, the SEN0137 hardware documentation,
and the open‑source embedded community.

---





