/* Minimal Cortex-M4 startup for the STICKER NFC bootloader.
 *
 * Only the 16 system exception vectors exist: the bootloader runs with PRIMASK
 * set and never takes a peripheral interrupt (a pending interrupt only wakes WFI),
 * so no IRQ vector is ever fetched. Any fault resets the chip. */

#include <stdint.h>

#include "stm32wle5xx.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int main(void);
void reset_handler(void);

static void fault_handler(void)
{
	NVIC_SystemReset();
}

__attribute__((section(".vectors"), used)) static void (*const m_vectors[16])(void) = {
	(void (*)(void))&_estack,
	reset_handler,
	fault_handler, /* NMI */
	fault_handler, /* HardFault */
	fault_handler, /* MemManage */
	fault_handler, /* BusFault */
	fault_handler, /* UsageFault */
	0,
	0,
	0,
	0,
	fault_handler, /* SVCall */
	fault_handler, /* DebugMon */
	0,
	fault_handler, /* PendSV */
	fault_handler, /* SysTick */
};

void reset_handler(void)
{
	uint32_t *src = &_sidata;

	for (uint32_t *dst = &_sdata; dst < &_edata;) {
		*dst++ = *src++;
	}
	for (uint32_t *dst = &_sbss; dst < &_ebss;) {
		*dst++ = 0;
	}

	main();

	NVIC_SystemReset();
}
