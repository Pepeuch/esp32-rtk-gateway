#pragma once

// Pinout conseillé pour ton PCB actuel

#define LORA_DEFAULT_MOSI      21
#define LORA_DEFAULT_MISO      17
#define LORA_DEFAULT_SCK       16
#define LORA_DEFAULT_NSS       15
#define LORA_DEFAULT_DIO1      18
#define LORA_DEFAULT_RESET     36
#define LORA_DEFAULT_BUSY      39

#define LORA_DEFAULT_SPI_CLOCK_HZ  8000000

#define LORA_DEFAULT_FREQ_HZ       869525000UL
#define LORA_DEFAULT_SF            7
#define LORA_DEFAULT_BW_HZ         500000
#define LORA_DEFAULT_CR            5
#define LORA_DEFAULT_SYNC_WORD     0x12
#define LORA_DEFAULT_TX_POWER_DBM  14
#define LORA_DEFAULT_PREAMBLE_LEN  8