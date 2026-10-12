/* STICKER NFC bootloader (#491) — bare-metal, no Zephyr, fits 4 KB.
 *
 * Boots the application at 0x08001000, or accepts a new application image over
 * the ST25DV FTM mailbox. No crypto, no image header, no metadata partition: the
 * application's vector table is the only validity marker, and its first flash
 * page is programmed last, after the image CRC passed. Any interrupted or failed
 * transfer therefore leaves an erased vector table and the next reset lands in
 * update mode again.
 *
 * Runs on MSI 4 MHz (reset clock) with PRIMASK set: interrupts only wake WFI
 * from Stop2, their handlers never run. See doc/plan/492 for the protocol. */

#include <stdbool.h>
#include <stdint.h>

#include "stm32wle5xx.h"

#define BOOT_VERSION 1

#define APP_ADDR  0x08001000UL
#define APP_END   0x08034000UL /* history partition */
#define SLOT_SIZE (APP_END - APP_ADDR)
#define PAGE_SIZE 2048UL

/* Last SRAM word, outside the bootloader's RAM window (boot.ld) and reserved in
 * the application: the app writes BOOT_MAGIC here and reboots (enter_dfu). */
#define BOOT_MAGIC_ADDR 0x2000FFFCUL
#define BOOT_MAGIC      0x21554644UL /* "DFU!" */

/* Update mode entered on request times out after this many idle LPTIM ticks. */
#define TICK_S         8
#define IDLE_TIMEOUT_S 240

/* ST25DV: user memory + dynamic registers on 0x53 (E0). */
#define ST25_ADDR        0x53
#define ST25_EH_CTRL     0x2002
#define ST25_FIELD_ON    0x04
#define ST25_MB_CTRL     0x2006
#define ST25_MB_EN       0x01
#define ST25_MB_HOST_PUT 0x02
#define ST25_MB_RF_PUT   0x04
#define ST25_MB_LEN      0x2007
#define ST25_MB_RAM      0x2008
#define ST25_RETRIES     20

/* Requests (first mailbox byte; distinct from the app's 0x01..0x03 channels). */
#define REQ_PING   0xB1
#define REQ_START  0xB2
#define REQ_DATA   0xB3
#define REQ_FINISH 0xB4

/* Reply: status(1) | expected offset(4, LE) | BOOT_VERSION(1). */
enum status {
	ST_OK = 0x00,
	ST_ERR_CMD = 0x01,
	ST_ERR_ARG = 0x02,
	ST_ERR_STATE = 0x03,
	ST_ERR_SEQ = 0x04,
	ST_ERR_FLASH = 0x05,
	ST_ERR_CRC = 0x06,
	ST_ERR_IMAGE = 0x07,
};

#define PIN_LPD   1  /* PA1: ST25DV low-power-down, low = tag powered */
#define PIN_LED_G 6  /* PA6: green LED, active low */
#define PIN_SCL   6  /* PB6: I2C1 SCL, AF4 */
#define PIN_SDA   7  /* PB7: I2C1 SDA, AF4 */
#define PIN_GPO   12 /* PB12: ST25DV GPO, EXTI12 */

#define FLASH_ERRS                                                                                 \
	(FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
	 FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR)

static uint32_t m_page[PAGE_SIZE / 4]; /* first slot page, programmed at FINISH */
static uint8_t m_rx[256];
static uint8_t m_tx[6];
static uint32_t m_len;
static uint32_t m_crc;
static uint32_t m_exp;
static bool m_started;

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool app_valid(const uint32_t *v)
{
	return v[0] > SRAM1_BASE && v[0] <= SRAM1_BASE + 0x10000 && (v[1] & 1) &&
	       v[1] >= APP_ADDR && v[1] < APP_END;
}

static void jump_to_app(void)
{
	SCB->VTOR = APP_ADDR;
	__asm volatile("msr msp, %0\n\tbx %1" ::"r"(((uint32_t *)APP_ADDR)[0]),
		       "r"(((uint32_t *)APP_ADDR)[1]));
}

static void iwdg_feed(void)
{
	/* The application starts the IWDG (10 s); it keeps running through the
	 * reboot into the bootloader and cannot be stopped. Harmless if not started. */
	IWDG->KR = 0xAAAA;
}

static void delay_ms(uint32_t ms)
{
	while (ms--) {
		iwdg_feed();
		while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk)) {
		}
	}
}

static void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode)
{
	port->MODER = (port->MODER & ~(3UL << (2 * pin))) | (mode << (2 * pin));
}

static void led(bool on)
{
	GPIOA->BSRR = on ? (1UL << (PIN_LED_G + 16)) : (1UL << PIN_LED_G);
}

/* ---- I2C1, polling ---------------------------------------------------------- */

static int i2c_wait(uint32_t flag)
{
	for (uint32_t n = 0; n < 100000; n++) {
		uint32_t isr = I2C1->ISR;

		if (isr & flag) {
			return 0;
		}
		if (isr & I2C_ISR_NACKF) {
			break; /* the master sends STOP by itself after a NACK */
		}
	}
	/* Software reset clears the state machine and every flag. */
	I2C1->CR1 = 0;
	I2C1->CR1 = I2C_CR1_PE;
	return -1;
}

/* One transaction on the 16-bit register space: write reg(+data) or reg then read. */
static int st25_xfer_once(uint16_t reg, uint8_t *buf, uint32_t len, bool read)
{
	uint32_t sadd = ST25_ADDR << 1;
	uint32_t n = read ? 2 : 2 + len;

	I2C1->CR2 = sadd | (n << I2C_CR2_NBYTES_Pos) | (read ? 0 : I2C_CR2_AUTOEND) | I2C_CR2_START;
	for (uint32_t i = 0; i < n; i++) {
		if (i2c_wait(I2C_ISR_TXIS)) {
			return -1;
		}
		I2C1->TXDR = i == 0 ? reg >> 8 : i == 1 ? reg & 0xFF : buf[i - 2];
	}
	if (read) {
		if (i2c_wait(I2C_ISR_TC)) {
			return -1;
		}
		I2C1->CR2 = sadd | I2C_CR2_RD_WRN | (len << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND |
			    I2C_CR2_START;
		for (uint32_t i = 0; i < len; i++) {
			if (i2c_wait(I2C_ISR_RXNE)) {
				return -1;
			}
			buf[i] = I2C1->RXDR;
		}
	}
	if (i2c_wait(I2C_ISR_STOPF)) {
		return -1;
	}
	I2C1->ICR = I2C_ICR_STOPCF;
	return 0;
}

/* RF owns the dual port while it talks to the tag: the tag NACKs, so retry. */
static int st25_xfer(uint16_t reg, uint8_t *buf, uint32_t len, bool read)
{
	for (int i = 0; i < ST25_RETRIES; i++) {
		if (!st25_xfer_once(reg, buf, len, read)) {
			return 0;
		}
		delay_ms(2);
	}
	return -1;
}

static int st25_read(uint16_t reg, uint8_t *buf, uint32_t len)
{
	/* NBYTES is 8 bits: split a full 256 B mailbox. */
	while (len) {
		uint32_t chunk = len > 128 ? 128 : len;

		if (st25_xfer(reg, buf, chunk, true)) {
			return -1;
		}
		reg += chunk;
		buf += chunk;
		len -= chunk;
	}
	return 0;
}

static int st25_write1(uint16_t reg, uint8_t v)
{
	return st25_xfer(reg, &v, 1, false);
}

/* ---- Flash ------------------------------------------------------------------- */

static int flash_wait(void)
{
	while (FLASH->SR & (FLASH_SR_BSY | FLASH_SR_CFGBSY)) {
	}
	uint32_t err = FLASH->SR & FLASH_ERRS;

	FLASH->SR = FLASH_ERRS;
	FLASH->CR = 0;
	return err ? -1 : 0;
}

static int flash_erase_page(uint32_t page)
{
	flash_wait();
	FLASH->CR = FLASH_CR_PER | (page << FLASH_CR_PNB_Pos);
	FLASH->CR |= FLASH_CR_STRT;
	return flash_wait();
}

static int flash_program(uint32_t addr, uint32_t lo, uint32_t hi)
{
	flash_wait();
	FLASH->CR = FLASH_CR_PG;
	*(volatile uint32_t *)addr = lo;
	__ISB();
	*(volatile uint32_t *)(addr + 4) = hi;
	return flash_wait();
}

/* ---- Protocol ---------------------------------------------------------------- */

static uint32_t image_crc(void)
{
	/* CRC-32 (IEEE 802.3, as zlib) on the CRC unit: word-reversed input and
	 * reversed output, init 0xFFFFFFFF, final XOR in software. */
	CRC->CR = CRC_CR_REV_IN | CRC_CR_REV_OUT | CRC_CR_RESET;

	uint32_t first = m_len < PAGE_SIZE ? m_len : PAGE_SIZE;

	for (uint32_t i = 0; i < first; i += 4) {
		CRC->DR = m_page[i / 4];
	}
	for (uint32_t a = APP_ADDR + PAGE_SIZE; a < APP_ADDR + m_len; a += 4) {
		CRC->DR = *(const uint32_t *)a;
	}
	return ~CRC->DR;
}

static enum status req_start(uint32_t n)
{
	if (n != 9) {
		return ST_ERR_ARG;
	}
	uint32_t len = rd32(&m_rx[1]);

	if (len == 0 || len > SLOT_SIZE || (len & 7)) {
		return ST_ERR_ARG;
	}

	m_started = false;
	uint32_t first = (APP_ADDR - FLASH_BASE) / PAGE_SIZE;
	uint32_t last = (APP_ADDR - FLASH_BASE + len - 1) / PAGE_SIZE;

	for (uint32_t p = first; p <= last; p++) {
		iwdg_feed();
		if (flash_erase_page(p)) {
			return ST_ERR_FLASH;
		}
	}
	/* Drop flash data-cache lines of the erased pages. */
	FLASH->ACR &= ~FLASH_ACR_DCEN;
	FLASH->ACR |= FLASH_ACR_DCRST;
	FLASH->ACR &= ~FLASH_ACR_DCRST;
	FLASH->ACR |= FLASH_ACR_DCEN;

	for (uint32_t i = 0; i < PAGE_SIZE / 4; i++) {
		m_page[i] = 0xFFFFFFFF;
	}
	m_len = len;
	m_crc = rd32(&m_rx[5]);
	m_exp = 0;
	m_started = true;
	return ST_OK;
}

static enum status req_data(uint32_t n)
{
	if (!m_started) {
		return ST_ERR_STATE;
	}
	if (n < 5 + 8 || ((n - 5) & 7)) {
		return ST_ERR_ARG;
	}
	uint32_t off = rd32(&m_rx[1]);
	uint32_t cnt = n - 5;

	if (off != m_exp) {
		/* Already written: a retried frame. Ahead: resume from m_exp. */
		return off < m_exp ? ST_OK : ST_ERR_SEQ;
	}
	if (off + cnt > m_len) {
		return ST_ERR_ARG;
	}
	for (uint32_t i = 0; i < cnt; i += 8) {
		uint32_t a = off + i;
		uint32_t lo = rd32(&m_rx[5 + i]);
		uint32_t hi = rd32(&m_rx[9 + i]);

		if (a < PAGE_SIZE) {
			m_page[a / 4] = lo;
			m_page[a / 4 + 1] = hi;
		} else if (flash_program(APP_ADDR + a, lo, hi)) {
			m_started = false;
			return ST_ERR_FLASH;
		}
	}
	m_exp += cnt;
	return ST_OK;
}

static enum status req_finish(void)
{
	if (!m_started || m_exp != m_len) {
		return ST_ERR_STATE;
	}
	if (image_crc() != m_crc) {
		return ST_ERR_CRC;
	}
	if (!app_valid(m_page)) {
		return ST_ERR_IMAGE;
	}
	/* From the end down: the first double-word (SP + reset vector) goes last, so
	 * a power loss in between leaves an erased, invalid vector table. */
	uint32_t first = m_len < PAGE_SIZE ? m_len : PAGE_SIZE;

	for (uint32_t a = first; a;) {
		a -= 8;
		if (flash_program(APP_ADDR + a, m_page[a / 4], m_page[a / 4 + 1])) {
			m_started = false;
			return ST_ERR_FLASH;
		}
	}
	m_started = false;
	return ST_OK;
}

/* Leave the tag as the application expects it (mailbox off, tag unpowered). */
static void leave(void)
{
	st25_write1(ST25_MB_CTRL, 0);
	GPIOA->BSRR = 1UL << PIN_LPD;
	NVIC_SystemReset();
}

/* Read one request, write its reply. Returns true when the new image is in. */
static bool serve(void)
{
	uint8_t lenm1;

	if (st25_read(ST25_MB_LEN, &lenm1, 1)) {
		return false;
	}
	uint32_t n = lenm1 + 1UL;

	if (st25_read(ST25_MB_RAM, m_rx, n)) {
		return false;
	}

	enum status st;

	switch (m_rx[0]) {
	case REQ_PING:
		st = ST_OK;
		break;
	case REQ_START:
		st = req_start(n);
		break;
	case REQ_DATA:
		st = req_data(n);
		break;
	case REQ_FINISH:
		st = req_finish();
		break;
	default:
		st = ST_ERR_CMD;
		break;
	}

	m_tx[0] = st;
	m_tx[1] = m_exp;
	m_tx[2] = m_exp >> 8;
	m_tx[3] = m_exp >> 16;
	m_tx[4] = m_exp >> 24;
	m_tx[5] = BOOT_VERSION;
	st25_xfer(ST25_MB_RAM, m_tx, sizeof(m_tx), false);

	return m_rx[0] == REQ_FINISH && st == ST_OK;
}

static void hw_init(void)
{
	/* 1 ms SysTick period on the 4 MHz reset clock, polled (no interrupt). */
	SysTick->LOAD = 4000 - 1;
	SysTick->VAL = 0;
	SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

	RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
	RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_I2C1EN | RCC_APB1ENR1_LPTIM1EN;
	(void)RCC->APB1ENR1;

	/* Tag powered, LED off. */
	GPIOA->BSRR = (1UL << (PIN_LPD + 16)) | (1UL << PIN_LED_G);
	gpio_mode(GPIOA, PIN_LPD, 1);
	gpio_mode(GPIOA, PIN_LED_G, 1);

	/* I2C1 on PB6/PB7, AF4 open-drain (external pull-ups), 100 kHz @ 4 MHz. */
	GPIOB->OTYPER |= (1UL << PIN_SCL) | (1UL << PIN_SDA);
	GPIOB->AFR[0] = (GPIOB->AFR[0] & ~0xFF000000UL) | 0x44000000UL;
	gpio_mode(GPIOB, PIN_SCL, 2);
	gpio_mode(GPIOB, PIN_SDA, 2);
	I2C1->TIMINGR = 0x00420F13;
	I2C1->CR1 = I2C_CR1_PE;

	/* GPO on PB12 (input, pull-down) wakes Stop2 via EXTI12 (rising). */
	gpio_mode(GPIOB, PIN_GPO, 0);
	GPIOB->PUPDR = (GPIOB->PUPDR & ~(3UL << (2 * PIN_GPO))) | (2UL << (2 * PIN_GPO));
	SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~7UL) | 1; /* EXTI12 = port B */
	EXTI->RTSR1 |= 1UL << PIN_GPO;
	EXTI->IMR1 |= (1UL << PIN_GPO) | EXTI_IMR1_IM29; /* + LPTIM1 wake line */
	NVIC_EnableIRQ(EXTI15_10_IRQn);

	/* LPTIM1 on LSI/128 = 250 Hz, wakes Stop2 every TICK_S to feed the IWDG. */
	RCC->CSR |= RCC_CSR_LSION;
	while (!(RCC->CSR & RCC_CSR_LSIRDY)) {
	}
	RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_LPTIM1SEL) | RCC_CCIPR_LPTIM1SEL_0;
	LPTIM1->CFGR = 7UL << LPTIM_CFGR_PRESC_Pos;
	LPTIM1->IER = LPTIM_IER_ARRMIE;
	LPTIM1->CR = LPTIM_CR_ENABLE;
	LPTIM1->ARR = 250 * TICK_S - 1;
	LPTIM1->CR |= LPTIM_CR_CNTSTRT;
	NVIC_EnableIRQ(LPTIM1_IRQn);
}

static void stop2(void)
{
	led(false);
	NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
	NVIC_ClearPendingIRQ(LPTIM1_IRQn);
	if (EXTI->PR1 & (1UL << PIN_GPO)) {
		return; /* GPO pulsed since the last check */
	}
	PWR->CR1 = (PWR->CR1 & ~PWR_CR1_LPMS) | PWR_CR1_LPMS_1;
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	__DSB();
	__WFI(); /* PRIMASK is set: wakes on the pending IRQ, never runs a handler */
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
}

static void update_mode(bool timeout)
{
	uint32_t idle = 0;

	__disable_irq();
	hw_init();
	delay_ms(150); /* tag boot after LPD low */

	for (;;) {
		iwdg_feed();
		EXTI->PR1 = 1UL << PIN_GPO;

		if (LPTIM1->ISR & LPTIM_ISR_ARRM) {
			LPTIM1->ICR = LPTIM_ICR_ARRMCF;
			if (timeout && ++idle >= IDLE_TIMEOUT_S / TICK_S) {
				leave(); /* nothing written yet: the old application boots */
			}
		}

		uint8_t ctrl = 0;
		uint8_t eh = 0;

		if (st25_read(ST25_MB_CTRL, &ctrl, 1) || st25_read(ST25_EH_CTRL, &eh, 1)) {
			stop2();
			continue;
		}
		if (!(ctrl & ST25_MB_EN)) {
			st25_write1(ST25_MB_CTRL, ST25_MB_EN);
		}
		if (ctrl & ST25_MB_RF_PUT) {
			led(true);
			idle = 0;
			if (serve()) {
				/* Let the phone read the final ACK, then boot the new image. */
				for (int i = 0; i < 100; i++) {
					delay_ms(10);
					if (!st25_read(ST25_MB_CTRL, &ctrl, 1) &&
					    !(ctrl & ST25_MB_HOST_PUT)) {
						break;
					}
				}
				leave();
			}
			continue;
		}
		if (eh & ST25_FIELD_ON) {
			/* Phone present: poll, do not depend on the GPO configuration. */
			led(true);
			delay_ms(2);
			continue;
		}
		stop2();
	}
}

int main(void)
{
	volatile uint32_t *magic = (volatile uint32_t *)BOOT_MAGIC_ADDR;
	bool requested = *magic == BOOT_MAGIC;
	bool valid = app_valid((const uint32_t *)APP_ADDR);

	*magic = 0;

	if (valid && !requested) {
		jump_to_app();
	}

	update_mode(valid);

	return 0;
}
