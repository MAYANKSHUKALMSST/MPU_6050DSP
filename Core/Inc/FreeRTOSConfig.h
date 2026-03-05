#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

/* ================= CORE ================= */

#define configENABLE_FPU                         1
#define configENABLE_MPU                         0

#define configUSE_PREEMPTION                     1
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         1

#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0

#define configCPU_CLOCK_HZ                       SystemCoreClock
#define configTICK_RATE_HZ                       ((TickType_t)1000)

/* ================= TASK CONFIG ================= */

#define configMAX_PRIORITIES                     10
#define configMINIMAL_STACK_SIZE                 256
#define configMAX_TASK_NAME_LEN                  16

/* ================= MEMORY ================= */

#define configTOTAL_HEAP_SIZE                    (64 * 1024)
#define configUSE_16_BIT_TICKS 0

/* ================= SYNCHRONIZATION ================= */

#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1

#define configQUEUE_REGISTRY_SIZE                8

/* ================= TRACE ================= */

#define configUSE_TRACE_FACILITY                 1

/* ================= TIMERS ================= */

#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                2
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             512

/* ================= NEWLIB ================= */

#define configUSE_NEWLIB_REENTRANT               1

/* ================= API FUNCTIONS ================= */

#define INCLUDE_vTaskPrioritySet             1
#define INCLUDE_uxTaskPriorityGet            1
#define INCLUDE_vTaskDelete                  1
#define INCLUDE_vTaskSuspend                 1
#define INCLUDE_vTaskDelayUntil              1
#define INCLUDE_vTaskDelay                   1
#define INCLUDE_xTaskGetSchedulerState       1
#define INCLUDE_xTimerPendFunctionCall       1
#define INCLUDE_xQueueGetMutexHolder         1
#define INCLUDE_uxTaskGetStackHighWaterMark  1
#define INCLUDE_eTaskGetState                1

/* ================= HEAP ================= */

#define USE_FreeRTOS_HEAP_4

/* ================= CORTEX M ================= */

#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY   15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#define configKERNEL_INTERRUPT_PRIORITY \
( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* ================= ASSERT ================= */

#define configASSERT(x) if((x)==0){taskDISABLE_INTERRUPTS();for(;;);}

/* ================= HANDLERS ================= */

#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif
