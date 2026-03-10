/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "debug_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
	DebugUART_Print("\r\n!!! HARD FAULT !!!\r\n");

	  uint32_t *sp;
	  __asm volatile(
	      "tst lr, #4\n"
	      "ite eq\n"
	      "mrseq %0, msp\n"
	      "mrsne %0, psp\n"
	      : "=r"(sp)
	  );

	  DebugUART_Print("R0  = 0x%08lX\r\n", sp[0]);
	  DebugUART_Print("R1  = 0x%08lX\r\n", sp[1]);
	  DebugUART_Print("R2  = 0x%08lX\r\n", sp[2]);
	  DebugUART_Print("R3  = 0x%08lX\r\n", sp[3]);
	  DebugUART_Print("R12 = 0x%08lX\r\n", sp[4]);
	  DebugUART_Print("LR  = 0x%08lX\r\n", sp[5]);
	  DebugUART_Print("PC  = 0x%08lX\r\n", sp[6]);
	  DebugUART_Print("PSR = 0x%08lX\r\n", sp[7]);

	  DebugUART_Print("HFSR = 0x%08lX\r\n", SCB->HFSR);
	  DebugUART_Print("CFSR = 0x%08lX\r\n", SCB->CFSR);
	  DebugUART_Print("MMFAR = 0x%08lX\r\n", SCB->MMFAR);
	  DebugUART_Print("BFAR = 0x%08lX\r\n", SCB->BFAR);
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  DebugUART_Print("\r\n!!! USAGE FAULT !!!\r\n");

  uint32_t *sp;
  __asm volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq %0, msp\n"
      "mrsne %0, psp\n"
      : "=r"(sp)
  );

  DebugUART_Print("R0  = 0x%08lX\r\n", sp[0]);
  DebugUART_Print("R1  = 0x%08lX\r\n", sp[1]);
  DebugUART_Print("R2  = 0x%08lX\r\n", sp[2]);
  DebugUART_Print("R3  = 0x%08lX\r\n", sp[3]);
  DebugUART_Print("R12 = 0x%08lX\r\n", sp[4]);
  DebugUART_Print("LR  = 0x%08lX\r\n", sp[5]);
  DebugUART_Print("PC  = 0x%08lX\r\n", sp[6]);
  DebugUART_Print("PSR = 0x%08lX\r\n", sp[7]);

  DebugUART_Print("HFSR = 0x%08lX\r\n", SCB->HFSR);
  DebugUART_Print("CFSR = 0x%08lX\r\n", SCB->CFSR);
  DebugUART_Print("UFSR = 0x%04lX\r\n", (SCB->CFSR >> 16) & 0xFFFF);

  while (1) { }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles Ethernet global interrupt.
  */
void ETH_IRQHandler(void)
{
  /* USER CODE BEGIN ETH_IRQn 0 */

  /* USER CODE END ETH_IRQn 0 */
  HAL_ETH_IRQHandler(&heth);
  /* USER CODE BEGIN ETH_IRQn 1 */

  /* USER CODE END ETH_IRQn 1 */
}

/* USER CODE BEGIN 1 */
/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  HAL_IncTick();

  /* FreeRTOS tick only after scheduler start */
#if (INCLUDE_xTaskGetSchedulerState == 1)
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
    xPortSysTickHandler();
  }
#else
  xPortSysTickHandler();
#endif
}
/* USER CODE END 1 */
