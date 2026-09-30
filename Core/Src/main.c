/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "lwip.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ethernet_task.h"
#include "debug_uart.h"
#include "task.h"
#include "eth_app.h"
#include "core_task.h"
#include "can_task.h"
#include <string.h>
#include "eth_raw_test.h"
#include "eth_loopback_test.h"

extern void ETH_DebugPrintCounters(const char *tag);
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

FDCAN_HandleTypeDef hfdcan1;
FDCAN_HandleTypeDef hfdcan2;

UART_HandleTypeDef huart3;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 8192,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_FDCAN1_Init(void);
static void MX_FDCAN2_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define DISABLE_UNALIGN_TRP 1

/*
 * 0 = обычный режим проекта: Ethernet + Core + CanTask + TCP/SLCAN.
 * 1 = автономный CAN-only тест:
 *     без Ethernet, без Core, без парсера.
 *     STM32 сама периодически отправляет CAN-кадры напрямую через FDCAN.
 */
#define CAN_ONLY_DIRECT_TEST 0

/*
 * 0 = тест на реальной CAN-шине, FDCAN_MODE_NORMAL.
 * 1 = внутренний loopback без физической шины.
 */
#define CAN_ONLY_DIRECT_TEST_LOOPBACK 0
#define CAN_ONLY_DIRECT_TEST_BITRATE 500000U

/*
 * 1 = диагностический raw-Ethernet тест (по просьбе начальника):
 *     отправка сырых кадров напрямую в обход TCP/UDP/IP.
 *     Не запускает обычный TCP-сервер/CAN-конвейер.
 */
#define ETH_RAW_LINK_TEST 0

/*
 * 1 = диагностический тест: плата отправляет и принимает данные САМА
 *     СЕБЕ через внутренний loopback PHY-микросхемы (LAN8742), без
 *     выхода в кабель и без участия внешнего клиента. Проверяет
 *     логику платы (DMA/TX/RX-пути) в изоляции от внешних факторов
 *     (кабель, коммутатор, сетевой стек ПК).
 */
#define ETH_LOOPBACK_TEST 0

static void CanOnlyDirectTest_Run(void);
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Disable unaligned access trap (temporary) */
  SCB->CCR &= ~SCB_CCR_UNALIGN_TRP_Msk;
  __DSB();
  __ISB();

  /* USER CODE BEGIN Init */
  static uint32_t boot_cnt = 0;
  boot_cnt++;
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks (FDCAN via PLL2) */
  PeriphCommonClock_Config();

  /* MPU Configuration -- после установки тактирования --------------------*/
  MPU_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART3_UART_Init();

  /* USER CODE BEGIN 2 */
  uint32_t rsr = RCC->RSR;
  (void)rsr;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  uint8_t msg[] = "\r\n=== SYSTEM START ===\r\n";
  HAL_UART_Transmit(&huart3, msg, sizeof(msg) - 1, 100);
  /* USER CODE END 2 */

  osKernelInitialize();

  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  osKernelStart();

  while (1) {}
}

/**
  * @brief System Clock Configuration
  * @retval None
  *
  * Источник -- HSI (внутренний генератор), а не HSE: на этой плате
  * выяснилось, что внешнего кварца/генератора на HSE физически нет,
  * контроллер незаметно переключался на HSI сам. Чтобы это было явным
  * и предсказуемым, HSE отключён совсем, PLL1 считается от HSI.
  *
  * HSI = 64 МГц, но в HAL_RCC_OscConfig ниже HSIState = RCC_HSI_DIV2,
  * поэтому на вход PLL1 (DIVM1) идёт 32 МГц:
  *   32 / PLLM(4) = 8 МГц (вход VCO)
  *   8 × PLLN(100) = 800 МГц (VCO)
  *   800 / PLLP(2) = 400 МГц = SYSCLK -- та же частота, что была раньше при HSE.
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV2;
  RCC_OscInitStruct.HSICalibrationValue = 64;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  /* FLASH_LATENCY_4 -- обязателен для SYSCLK=400 МГц на VOS1, иначе
   * ядро читает флеш быстрее, чем она успевает отвечать -- Hard Fault
   * почти сразу после старта. */
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Peripherals Common Clock Configuration -- FDCAN через PLL2
  * @retval None
  *
  * Тоже теперь от HSI. Вход на PLL2 после DIVM2(4) = 8 МГц (то же самое
  * HSI/2, что и на PLL1 выше):
  *   8 × PLL2N(25) = 200 МГц (VCO)
  *   200 / PLL2Q(4) = 50 МГц = fdcan_ker_ck -- та же частота, что и
  *   раньше при HSE, поэтому CanTask_GetBitTiming() в can_task.c
  *   менять не нужно, все битрейты (125k/250k/500k/800k/1M) остаются
  *   верными как есть.
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
  PeriphClkInitStruct.PLL2.PLL2M = 4;
  PeriphClkInitStruct.PLL2.PLL2N = 25;
  PeriphClkInitStruct.PLL2.PLL2P = 2;
  PeriphClkInitStruct.PLL2.PLL2Q = 4;
  PeriphClkInitStruct.PLL2.PLL2R = 2;
  PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_3;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
  PeriphClkInitStruct.FdcanClockSelection = RCC_FDCANCLKSOURCE_PLL2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief FDCAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN1_Init(void)
{
  FDCAN_FilterTypeDef sFilterConfig = {0};

  /*
   * Конфигурация FDCAN взята из прошивки Славы.
   * Отличие: запуск HAL_FDCAN_Start() выполняется не здесь,
   * а в CanTask_Open(), потому что канал открывается по SLCAN-команде O/L/Y.
   */

  hfdcan1.Instance = FDCAN1;
  hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;  //обычные кадры до 8 байт данных
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL; //режим, можно менять командами O, L, Y
  hfdcan1.Init.AutoRetransmission = DISABLE; //повторная передача
  /*
     * если CAN-кадр не был подтверждён на шине,
     * FDCAN не будет бесконечно пытаться отправлять его повторно.
     */
  hfdcan1.Init.TransmitPause = DISABLE;
  hfdcan1.Init.ProtocolException = ENABLE;

  /*
   * начальная конфигурация FDCAN
   * настройка для 1 Мбит/с (значение по умолчанию)
   * при fdcan_ker_ck = 50 МГц.
   */
  hfdcan1.Init.NominalPrescaler = 1;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 48;
  hfdcan1.Init.NominalTimeSeg2 = 1;

  hfdcan1.Init.DataPrescaler = 13;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 2;
  hfdcan1.Init.DataTimeSeg2 = 1;

  /*
   * Настройки Message RAM, FIFO и TX очереди.
   */
  hfdcan1.Init.MessageRAMOffset = 0; //начать размещать структуры FDCAN в Message RAM с начала области

  hfdcan1.Init.StdFiltersNbr = 1; //выделяем 1 фильтр для standard ID
  hfdcan1.Init.ExtFiltersNbr = 1; //выделяем 1 фильтр для extended ID

  // первая очередь
  hfdcan1.Init.RxFifo0ElmtsNbr = 8;  //размер очереди = 8 кадров (у Славы было 1)
  hfdcan1.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;

  // вторая очередь (не используется)
  hfdcan1.Init.RxFifo1ElmtsNbr = 2;
  hfdcan1.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;

  //отдельный фиксированный буфер (не используется)
  hfdcan1.Init.RxBuffersNbr = 0;  // 0 = выделенных RX buffers нет
  hfdcan1.Init.RxBufferSize = FDCAN_DATA_BYTES_8;

  hfdcan1.Init.TxEventsNbr = 1; // очередь событий передачи
  hfdcan1.Init.TxBuffersNbr = 0; // буфер (не используется)

  hfdcan1.Init.TxFifoQueueElmtsNbr = 8; //размер очереди = 8 кадров (у Славы было 1)
  hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan1.Init.TxElmtSize = FDCAN_DATA_BYTES_8;

  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * Фильтр для итогового CAN-Ethernet преобразователя:
   * принимаем все стандартные CAN ID в RX FIFO0.
   */
  sFilterConfig.IdType = FDCAN_STANDARD_ID;
  sFilterConfig.FilterIndex = 0;
  sFilterConfig.FilterType = FDCAN_FILTER_MASK;
  sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  sFilterConfig.FilterID1 = 0x000;
  sFilterConfig.FilterID2 = 0x000;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * Extended CAN ID тоже принимаем все в RX FIFO0.
   */
  sFilterConfig.IdType = FDCAN_EXTENDED_ID;
  sFilterConfig.FilterIndex = 0;
  sFilterConfig.FilterType = FDCAN_FILTER_MASK;
  sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  sFilterConfig.FilterID1 = 0x00000000;
  sFilterConfig.FilterID2 = 0x00000000;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * Глобальный фильтр:
   * unmatched standard и extended кадры тоже принимаем в RX FIFO0,
   * remote frames отклоняем.
   */
  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
                                   FDCAN_ACCEPT_IN_RX_FIFO0,
                                   FDCAN_ACCEPT_IN_RX_FIFO0,
                                   FDCAN_REJECT_REMOTE,
                                   FDCAN_REJECT_REMOTE) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * В прошивке Славы приём был через polling.
   * В моем проекте приём идёт через callback, поэтому здесь
   * дополнительно назначаем прерывание RX FIFO0 на interrupt line 0.
   */
  if (HAL_FDCAN_ConfigInterruptLines(&hfdcan1,
                                     FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                     FDCAN_INTERRUPT_LINE0) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                     FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                     0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief FDCAN2 Initialization Function
  * @param None
  * @retval None
  *
  * Второй физический CAN-канал. Пока не используется приложением
  * (нет CanTask/клиентов для него) -- инициализация только на уровне
  * периферии, без настройки фильтров/прерываний (это будет добавлено,
  * когда канал реально понадобится).
  */
static void MX_FDCAN2_Init(void)
{
  hfdcan2.Instance = FDCAN2;
  hfdcan2.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan2.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan2.Init.AutoRetransmission = DISABLE;
  hfdcan2.Init.TransmitPause = DISABLE;
  hfdcan2.Init.ProtocolException = DISABLE;

  /*
   * Такая же начальная конфигурация, как у FDCAN1 -- 1 Мбит/с при
   * fdcan_ker_ck = 50 МГц. Реальный битрейт всё равно переустанавливается
   * программно при каждом открытии канала, эти значения тут только для
   * самой первой инициализации при старте платы.
   */
  hfdcan2.Init.NominalPrescaler = 1;
  hfdcan2.Init.NominalSyncJumpWidth = 1;
  hfdcan2.Init.NominalTimeSeg1 = 48;
  hfdcan2.Init.NominalTimeSeg2 = 1;

  hfdcan2.Init.DataPrescaler = 13;
  hfdcan2.Init.DataSyncJumpWidth = 1;
  hfdcan2.Init.DataTimeSeg1 = 2;
  hfdcan2.Init.DataTimeSeg2 = 1;

  /*
   * ВАЖНО: MessageRAMOffset = 77 -- ровно там, где заканчивается область
   * FDCAN1 (1 std-фильтр(1) + 1 ext-фильтр(2) + RxFifo0(8*4=32) +
   * RxFifo1(2*4=8) + TxEvents(1*2=2) + TxFifoQueue(8*4=32) = 77 слов).
   * Если поменяешь размеры очередей/фильтров у FDCAN1 -- пересчитай
   * и это смещение заново, иначе каналы начнут портить данные друг друга.
   */
  hfdcan2.Init.MessageRAMOffset = 77;

  hfdcan2.Init.StdFiltersNbr = 1;
  hfdcan2.Init.ExtFiltersNbr = 1;

  hfdcan2.Init.RxFifo0ElmtsNbr = 8;
  hfdcan2.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;

  hfdcan2.Init.RxFifo1ElmtsNbr = 2;
  hfdcan2.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;

  hfdcan2.Init.RxBuffersNbr = 0;
  hfdcan2.Init.RxBufferSize = FDCAN_DATA_BYTES_8;

  hfdcan2.Init.TxEventsNbr = 1;
  hfdcan2.Init.TxBuffersNbr = 0;

  hfdcan2.Init.TxFifoQueueElmtsNbr = 8;
  hfdcan2.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan2.Init.TxElmtSize = FDCAN_DATA_BYTES_8;

  if (HAL_FDCAN_Init(&hfdcan2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */
  /* Включаем тактирование USART3 */
  __HAL_RCC_USART3_CLK_ENABLE();
  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */
  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*
   * PB0 — как в рабочем CAN-проекте.
   * Скорее всего, это EN/STB/Silent управление CAN-трансивером.
   */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
static void CanOnlyDirectTest_Run(void)
{
  FDCAN_FilterTypeDef filter = {0};  //фильтр, какие принимаем кадры
  FDCAN_TxHeaderTypeDef txHeader = {0}; //Заголовок передаваемого CAN-кадра.
  uint8_t txData[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}; //Массив данных CAN-кадра.
  uint32_t counter = 0; //Счётчик отправленных кадров

  DebugUART_Print("[CAN_ONLY] direct CAN test started\r\n");

  // На всякий случай останавливаем FDCAN, если он был запущен.
  (void)HAL_FDCAN_Stop(&hfdcan1);

  /*
   * Выбор режима:
   * - NORMAL для реальной CAN-шины;
   * - INTERNAL_LOOPBACK для проверки без физики.
   */
#if CAN_ONLY_DIRECT_TEST_LOOPBACK
  hfdcan1.Init.Mode = FDCAN_MODE_INTERNAL_LOOPBACK;
  DebugUART_Print("[CAN_ONLY] mode = INTERNAL LOOPBACK\r\n");
#else
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
  DebugUART_Print("[CAN_ONLY] mode = NORMAL physical CAN\r\n");
#endif

  /*
   * Настройка скорости.
   * Если fdcan_ker_ck = 50 МГц:
   * 50 000 000 / (1 × (1 + 86 + 13)) = 500 000 бит/с
   */
#if (CAN_ONLY_DIRECT_TEST_BITRATE == 500000U)
  hfdcan1.Init.NominalPrescaler = 1;
  hfdcan1.Init.NominalSyncJumpWidth = 13;
  hfdcan1.Init.NominalTimeSeg1 = 86;
  hfdcan1.Init.NominalTimeSeg2 = 13;

  //Data phase для CAN FD.
  hfdcan1.Init.DataPrescaler = 25;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 2;
  hfdcan1.Init.DataTimeSeg2 = 1;

  DebugUART_Print("[CAN_ONLY] bitrate = 500 kbit/s\r\n");

#elif (CAN_ONLY_DIRECT_TEST_BITRATE == 1000000U)
  /*
     * Настройка nominal phase для 1 Мбит/с.
     *
     * Если fdcan_ker_ck = 50 МГц:
     * 50 000 000 / (1 × (1 + 48 + 1)) = 1 000 000 бит/с
     */
  hfdcan1.Init.NominalPrescaler = 1;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 48;
  hfdcan1.Init.NominalTimeSeg2 = 1;

  hfdcan1.Init.DataPrescaler = 13;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 2;
  hfdcan1.Init.DataTimeSeg2 = 1;

  DebugUART_Print("[CAN_ONLY] bitrate = 1 Mbit/s\r\n");

#else
  DebugUART_Print("[CAN_ONLY] ERROR: unsupported CAN_ONLY_DIRECT_TEST_BITRATE\r\n");
  for (;;)
  {
    osDelay(1000);
  }
#endif

  /*
   * Повторная инициализация FDCAN с выбранным режимом и скоростью.
   */
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: HAL_FDCAN_Init failed\r\n");
    Error_Handler();
  }

  /*
   * Принимаем все стандартные ID.
   */
  filter.IdType = FDCAN_STANDARD_ID;
  filter.FilterIndex = 0;
  filter.FilterType = FDCAN_FILTER_MASK;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = 0x000;
  filter.FilterID2 = 0x000;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: standard filter config failed\r\n");
    Error_Handler();
  }

  /*
   * Принимаем все extended ID.
   */
  filter.IdType = FDCAN_EXTENDED_ID;
  filter.FilterIndex = 0;
  filter.FilterType = FDCAN_FILTER_MASK;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = 0x00000000;
  filter.FilterID2 = 0x00000000;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: extended filter config failed\r\n");
    Error_Handler();
  }

  /*
   * Unmatched кадры принимаем в FIFO0,
   * remote frames отклоняем.
   */
  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
                                   FDCAN_ACCEPT_IN_RX_FIFO0,
                                   FDCAN_ACCEPT_IN_RX_FIFO0,
                                   FDCAN_REJECT_REMOTE,
                                   FDCAN_REJECT_REMOTE) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: global filter config failed\r\n");
    Error_Handler();
  }

  /*
     * Настраиваем, что событие "новое сообщение в RX FIFO0"
     * будет идти на interrupt line 0.
     */
  if (HAL_FDCAN_ConfigInterruptLines(&hfdcan1,
                                     FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                     FDCAN_INTERRUPT_LINE0) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: interrupt line config failed\r\n");
    Error_Handler();
  }

  /*
     * Активируем уведомление о новом сообщении в RX FIFO0.
     * После этого при приёме кадра может вызываться HAL_FDCAN_RxFifo0Callback().
     */
  if (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                     FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                     0) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: notification activate failed\r\n");
    Error_Handler();
  }

  /*
     * Запуска FDCAN
     */
  if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
  {
    DebugUART_Print("[CAN_ONLY] ERROR: HAL_FDCAN_Start failed\r\n");
    Error_Handler();
  }

  DebugUART_Print("[CAN_ONLY] FDCAN started\r\n");
  DebugUART_Print("[CAN_ONLY] FDCAN kernel clock=%lu\r\n",
                  (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN));
  DebugUART_Print("[CAN_ONLY] NBTP=0x%08lX PSR=0x%08lX CCCR=0x%08lX\r\n",
                  (unsigned long)hfdcan1.Instance->NBTP,
                  (unsigned long)hfdcan1.Instance->PSR,
                  (unsigned long)hfdcan1.Instance->CCCR);

  /*
   * Заголовок CAN-кадра.
   * Отправляем standard ID = 0x123, DLC = 8.
   */
  txHeader.Identifier = 0x123;
  txHeader.IdType = FDCAN_STANDARD_ID; //ID стандартный 11 бит
  txHeader.TxFrameType = FDCAN_DATA_FRAME;
  txHeader.DataLength = FDCAN_DLC_BYTES_8;	//Длина данных 8 байт
  txHeader.ErrorStateIndicator = FDCAN_ESI_PASSIVE;
  txHeader.BitRateSwitch = FDCAN_BRS_OFF;
  txHeader.FDFormat = FDCAN_CLASSIC_CAN;
  txHeader.TxEventFifoControl = FDCAN_STORE_TX_EVENTS; //Сохранять событие передачи в TX Event FIFO
  txHeader.MessageMarker = 0xDD; //Маркер сообщения

  /*
   * Бесконечный цикл прямой отправки CAN-кадров.
   */
  for (;;)
  {
    /*
     * Для наглядности меняем первый байт данных.
     */
    txData[0] = (uint8_t)(counter & 0xFFU);

    if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0U)
    {
      if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txHeader, txData) == HAL_OK)
      {
        DebugUART_Print("[CAN_ONLY] TX #%lu ID=0x123 DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                        (unsigned long)counter,
                        txData[0], txData[1], txData[2], txData[3],
                        txData[4], txData[5], txData[6], txData[7]);
      }
      else
      {
        DebugUART_Print("[CAN_ONLY] ERROR: AddMessage failed, err=0x%08lX PSR=0x%08lX ECR=0x%08lX\r\n",
                        (unsigned long)HAL_FDCAN_GetError(&hfdcan1),
                        (unsigned long)hfdcan1.Instance->PSR,
                        (unsigned long)hfdcan1.Instance->ECR);
      }
    }
    else
    {
      DebugUART_Print("[CAN_ONLY] TX FIFO FULL TXFQS=0x%08lX PSR=0x%08lX ECR=0x%08lX\r\n",
                      (unsigned long)hfdcan1.Instance->TXFQS,
                      (unsigned long)hfdcan1.Instance->PSR,
                      (unsigned long)hfdcan1.Instance->ECR);
    }

    counter++;

    /*
     * Период отправки = 1 кадр в секунду
     */
    osDelay(1000);
  }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  (void)argument;

  DebugUART_InitMutex();

  SCB->CCR &= ~SCB_CCR_UNALIGN_TRP_Msk;
  __DSB();
  __ISB();

  /* init lwIP */
  MX_LWIP_Init();

  /* init FDCAN before CAN task start */
  MX_FDCAN1_Init();
  MX_FDCAN2_Init();

  /* USER CODE BEGIN FDCAN clock debug */
    DebugUART_Print("[CLOCK] FDCAN kernel clock=%lu Hz\r\n",
                    (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN));
    /* USER CODE END FDCAN clock debug */

    /* USER CODE BEGIN HSI MCO debug */
      {
        GPIO_InitTypeDef GPIO_InitStruct = {0};

        __HAL_RCC_GPIOA_CLK_ENABLE();

        // PA8 на Nucleo-H723ZG выведена на разъём CN10, pin 23
        GPIO_InitStruct.Pin       = GPIO_PIN_8;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* Источник HSI  */
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);

        DebugUART_Print("[CLOCK] MCO1 (PA8) configured, source=HSI, div=1 -- measure with scope on CN10 pin23\r\n");
      }
      /* USER CODE END HSI MCO debug */

#if CAN_ONLY_DIRECT_TEST
  CanOnlyDirectTest_Run();
#elif ETH_RAW_LINK_TEST
  EthRawTest_Start();

  for (;;)
  {
    osDelay(1000);
  }
#elif ETH_LOOPBACK_TEST
  /*
   * Loopback-тест не требует CAN/TCP-конвейера — MX_LWIP_Init() уже
   * выполнена выше (запустила heth, RxPktSemaphore/TxPktSemaphore,
   * EthTxTask и ethernetif_input), этого достаточно для работы теста.
   */
  EthLoopbackTest_Start();

  for (;;)
  {
    osDelay(1000);
  }
#else
  /* pipeline */
  EthApp_Init();
  CoreTask_Start();
  //CanTask_Start();
  CanTask_Start(0);
  CanTask_Start(1);

  /* Ethernet app task */
  EthernetTask_Start();

  for (;;)
  {
    osDelay(1000);
  }
#endif
}

 /* MPU Configuration */
void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  HAL_MPU_Disable();

  /* -------- Region 0: AXI SRAM (D1) 0x24000000, 320KB, Cacheable WB/WA -------- */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress      = 0x24000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_512KB;   // ближайший степенной размер
  MPU_InitStruct.SubRegionDisable = 0xE0;

  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* -------- Region 1: SRAM D2 (0x30000000, 32KB) NON-CACHEABLE for ETH DMA -------- */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress      = 0x30000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_32KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* (опционально) Region 2: SRAM D3 (0x38000000, 16KB) cacheable можно */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress      = 0x38000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_16KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  /* Включаем кеши (после MPU!) */
  SCB_EnableICache();
  SCB_EnableDCache();    /* временно выключено для отладки Ethernet */
  __DSB();
  __ISB();
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */

void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
