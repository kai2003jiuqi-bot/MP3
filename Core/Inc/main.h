/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define INC_VOLUME_Pin GPIO_PIN_4
#define INC_VOLUME_GPIO_Port GPIOC
#define DEC_VOLUME_Pin GPIO_PIN_5
#define DEC_VOLUME_GPIO_Port GPIOC
#define LCD_CS_Pin GPIO_PIN_12
#define LCD_CS_GPIO_Port GPIOB
#define LCD_RST_Pin GPIO_PIN_13
#define LCD_RST_GPIO_Port GPIOB
#define LCD_DC_Pin GPIO_PIN_14
#define LCD_DC_GPIO_Port GPIOB
#define LCD_LED_Pin GPIO_PIN_15
#define LCD_LED_GPIO_Port GPIOB
#define T_CS_Pin GPIO_PIN_6
#define T_CS_GPIO_Port GPIOC
#define T_IRQ_Pin GPIO_PIN_7
#define T_IRQ_GPIO_Port GPIOC
#define VS1003_CS_Pin GPIO_PIN_5
#define VS1003_CS_GPIO_Port GPIOB
#define VS1003_DCS_Pin GPIO_PIN_6
#define VS1003_DCS_GPIO_Port GPIOB
#define VS1003_DREQ_Pin GPIO_PIN_7
#define VS1003_DREQ_GPIO_Port GPIOB
#define VS1003_RST_Pin GPIO_PIN_8
#define VS1003_RST_GPIO_Port GPIOB
#define W25Q64_CS_Pin GPIO_PIN_9
#define W25Q64_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
