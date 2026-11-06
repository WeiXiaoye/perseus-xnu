/*
 * The touch service: ST FTS (FTM5) on QUP SE14 of the Xiaomi MIX 3, driven from the loader.
 *
 * XNU cannot own this hardware: the I/O rail is a TLMM pin, AVDD is PM8998 L28 (an RPMh write
 * from the kernel resets the machine) and the bus is a QUP the firmware will not hand over. The
 * loader keeps UEFI alive on the boot core for as long as the kernel runs, so the chip is driven
 * here and the kernel only reads the page below.
 *
 * Sequence that makes the controller scan (measured, see docs/touch-uefi.md):
 *   GPIO79 high -> RPMh request for L28 -> IER write -> reset -> "controller ready"
 *   -> read sysinfo -> reset again -> flush FIFO -> scan mode active.
 */
#include "efi.h"

/* off with touch=0 on the kernel command line (parsed where the loader reads pi.cmdline) */
static int touch_enabled = 1;

/* ---- the firmware protocols this needs ---- */
static const efi_guid g_tlmm_protocol = { 0xad9aec18, 0x7bf0, 0x4809,
					  { 0x9e, 0x96, 0x30, 0x12, 0x30, 0x9f, 0x3d, 0xf7 } };

struct qcom_tlmm {
	uint64_t revision;
	efi_status (EFIAPI *config_gpio)(uint32_t config, uint32_t enable);
	efi_status (EFIAPI *config_gpio_group)(uint32_t enable, uint32_t *group, uint32_t size);
	efi_status (EFIAPI *gpio_in)(uint32_t config, uint32_t *value);
	efi_status (EFIAPI *gpio_out)(uint32_t config, uint32_t value);
	efi_status (EFIAPI *set_inactive)(uint32_t gpio, uint32_t config);
};

static struct qcom_tlmm *touch_tlmm;

#define TOUCH_PIN_INT   31
#define TOUCH_PIN_RESET 32
#define TOUCH_PIN_SDA   33
#define TOUCH_PIN_SCL   34
#define TOUCH_PIN_VDDIO 79
#define TOUCH_ADDR      0x49

#define TOUCH_GPIO_CFG(pin, func, dir, pull, drive)                                            \
	((((uint32_t)(pin) & 0x3ff) << 4) | (((uint32_t)(func) & 0xf)) |                      \
	 ((((uint32_t)(dir)) & 1) << 14) | ((((uint32_t)(pull)) & 3) << 15) |                  \
	 ((((uint32_t)(drive)) & 0xf) << 17) | 0x20000000u)

#define T_INPUT  0
#define T_OUTPUT 1
#define T_NOPULL 0
#define T_PULLUP 3

/* ---- shared page: the kernel reads this, it names it on its command line ---- */
#define TOUCH_MAGIC 0x43554f54u /* 'TOUC' */
#define TOUCH_DOWN  1
#define TOUCH_MOVE  2
#define TOUCH_UP    3

struct touch_sample {
	uint32_t type, finger, x, y; /* x,y in 0..65535 */
};

struct touch_page {
	volatile uint32_t magic;
	volatile uint32_t seq;   /* written last, the kernel polls this */
	volatile uint32_t count; /* samples written so far */
	volatile uint32_t down;  /* fingers currently down (a bit per finger) */
	struct touch_sample samples[64];
};

static struct touch_page touch_shared __attribute__((aligned(4096)));
static int touch_ready;


static uint32_t touch_int_level(void)
{
	uint32_t value = 1;

	touch_tlmm->gpio_in(TOUCH_GPIO_CFG(TOUCH_PIN_INT, 0, T_INPUT, T_PULLUP, 0), &value);
	return value & 1;
}
static void touch_us_delay(uint32_t us)
{
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), start = READ_SYSREG(CNTVCT_EL0);

	while ((READ_SYSREG(CNTVCT_EL0) - start) < (freq / 1000000ULL) * us) {
	}
}

/* ---- bit-banged I2C on GPIO33/34: SE14's QUP is not usable under this firmware ---- */
#define BB_DELAY_US 5

static void bb_delay(void) { touch_us_delay(BB_DELAY_US); }

static void sda_low(void)
{
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_SDA, 0, T_OUTPUT, T_NOPULL, 0), 1);
	touch_tlmm->gpio_out(TOUCH_GPIO_CFG(TOUCH_PIN_SDA, 0, T_OUTPUT, T_NOPULL, 0), 0);
}

static void sda_release(void)
{
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_SDA, 0, T_INPUT, T_PULLUP, 0), 1);
}

static uint32_t sda_read(void)
{
	uint32_t value = 1;

	touch_tlmm->gpio_in(TOUCH_GPIO_CFG(TOUCH_PIN_SDA, 0, T_INPUT, T_PULLUP, 0), &value);
	return value & 1;
}

static void scl_low(void)
{
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_SCL, 0, T_OUTPUT, T_NOPULL, 0), 1);
	touch_tlmm->gpio_out(TOUCH_GPIO_CFG(TOUCH_PIN_SCL, 0, T_OUTPUT, T_NOPULL, 0), 0);
}

static void scl_release(void)
{
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_SCL, 0, T_INPUT, T_PULLUP, 0), 1);
}

static void bb_start(void)
{
	sda_release(); scl_release(); bb_delay();
	sda_low(); bb_delay();
	scl_low(); bb_delay();
}

static void bb_stop(void)
{
	sda_low(); bb_delay();
	scl_release(); bb_delay();
	sda_release(); bb_delay();
}

static int bb_put(uint8_t byte)
{
	for (int i = 0; i < 8; i++) {
		if (byte & 0x80) {
			sda_release();
		} else {
			sda_low();
		}
		byte <<= 1;
		bb_delay();
		scl_release(); bb_delay();
		scl_low(); bb_delay();
	}
	sda_release(); bb_delay();
	scl_release(); bb_delay();
	int ack = !sda_read();
	scl_low(); bb_delay();
	return ack;
}

static uint8_t bb_get(int ack)
{
	uint8_t value = 0;

	sda_release();
	for (int i = 0; i < 8; i++) {
		bb_delay();
		scl_release(); bb_delay();
		value = (uint8_t)(value << 1 | sda_read());
		scl_low(); bb_delay();
	}
	if (ack) {
		sda_low();
	} else {
		sda_release();
	}
	bb_delay();
	scl_release(); bb_delay();
	scl_low(); bb_delay();
	sda_release();
	return value;
}

/* a command with its own stop (system commands, hardware register writes) */
static int touch_write(const uint8_t *data, uint32_t length)
{
	bb_start();
	if (!bb_put(TOUCH_ADDR << 1)) {
		bb_stop();
		return 0;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (!bb_put(data[i])) {
			bb_stop();
			return 0;
		}
	}
	bb_stop();
	return 1;
}

/* the FTM5 order: command without a stop, then the read starts again */
static int touch_read(const uint8_t *command, uint32_t count, uint8_t *data, uint32_t length)
{
	bb_start();
	if (!bb_put(TOUCH_ADDR << 1)) {
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!bb_put(command[i])) {
			return 0;
		}
	}
	bb_start();
	if (!bb_put((TOUCH_ADDR << 1) | 1)) {
		bb_stop();
		return 0;
	}
	for (uint32_t i = 0; i < length; i++) {
		data[i] = bb_get(i + 1 < length);
	}
	bb_stop();
	return 1;
}

static void touch_recover(void)
{
	sda_release();
	for (int i = 0; i < 9; i++) {
		scl_low(); bb_delay();
		scl_release(); bb_delay();
	}
	bb_stop();
}

/* ---- FTM5 ---- */
#define FTM5_READ_EVENT  0x86
#define FTM5_HW_REG      0xfa
#define FTM5_FRAMEBUFFER 0xa6
#define FTM5_SCAN_MODE   0xa0
#define FTM5_SYSTEM      0xa4
#define EVT_READY        0x03
#define EVT_STATUS       0x43

static int touch_wait_event(uint8_t want, uint32_t timeout_ms, uint8_t *event)
{
	static const uint8_t command = FTM5_READ_EVENT;

	for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
		if (touch_read(&command, 1, event, 8) && event[0] == want) {
			return 1;
		}
		touch_recover();
		touch_us_delay(10000);
	}
	return 0;
}

static int touch_command(const uint8_t *command, uint32_t length)
{
	uint8_t want[8] = { EVT_STATUS, 0x01, 0, 0, 0, 0, 0, 0 }, event[8];
	uint32_t n = length > 5 ? 5 : length;

	if (!touch_write(command, length)) {
		touch_recover();
		return 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		want[2 + i] = command[i];
	}
	for (uint32_t waited = 0; waited < 500; waited += 10) {
		static const uint8_t read_event = FTM5_READ_EVENT;

		if (touch_read(&read_event, 1, event, 8) && memcmp(event, want, 2 + n) == 0) {
			return 1;
		}
		touch_us_delay(10000);
	}
	return 0;
}

static int touch_hw_write(uint32_t address, uint8_t value)
{
	uint8_t command[6] = { FTM5_HW_REG, (uint8_t)(address >> 24), (uint8_t)(address >> 16),
			       (uint8_t)(address >> 8), (uint8_t)address, value };

	return touch_write(command, sizeof(command));
}

static int touch_hw_read(uint32_t address, uint8_t *buffer, uint32_t length)
{
	uint8_t command[5] = { FTM5_HW_REG, (uint8_t)(address >> 24), (uint8_t)(address >> 16),
			       (uint8_t)(address >> 8), (uint8_t)address };

	return touch_read(command, 5, buffer, length);
}

/* ---- RPMh: AVDD is pm8998_l28, and this is the one call the kernel cannot make ---- */
#define RSC_PHYS    0x179e0000UL
#define CMD_DB_PHYS 0x85fe0000UL
#define TCS(reg)    (RSC_PHYS + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};

struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t rpmh_address(const char *name)
{
	const uint8_t *db = (const uint8_t *)(uintptr_t)CMD_DB_PHYS;
	char id[8] = { 0 };

	if (db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c) {
		return 0;
	}
	for (int i = 0; i < 7 && name[i]; i++) {
		id[i] = name[i];
	}
	for (int s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = (const struct cmd_db_slave *)(db + 8 + s * sizeof(*slave));
		const struct cmd_db_entry *entry;
		uint32_t base;

		if (slave->id == 0 || slave->header_offset >= 0x1f000 || slave->count > 512) {
			continue;
		}
		base = 8 + 8 * (uint32_t)sizeof(struct cmd_db_slave) + 8 + slave->header_offset;
		for (uint32_t i = 0; i < slave->count; i++) {
			entry = (const struct cmd_db_entry *)(db + base + i * sizeof(*entry));
			if (memcmp(entry->id, id, sizeof(id)) == 0) {
				return entry->address;
			}
		}
	}
	return 0;
}

static uint32_t mmio_read(uint64_t address) { return *(volatile uint32_t *)(uintptr_t)address; }

static void mmio_write(uint64_t address, uint32_t value) { *(volatile uint32_t *)(uintptr_t)address = value; }

static int touch_power_avdd(void)
{
	uint32_t base = rpmh_address("ldoa28");
	uint32_t address[3], data[3] = { 3300, 1, 7 };
	uint32_t irq_enable, i;

	if (base == 0) {
		return 0;
	}
	address[0] = base;
	address[1] = base + 4;
	address[2] = base + 8;
	for (i = 0; i < 2000 && mmio_read(TCS(TCS_STATUS)) == 0; i++) {
		touch_us_delay(100);
	}
	irq_enable = mmio_read(TCS(TCS_IRQ_ENABLE));
	mmio_write(TCS(TCS_CONTROL), mmio_read(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	mmio_write(TCS(TCS_CONTROL), mmio_read(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	mmio_write(TCS(TCS_IRQ_CLEAR), 1);
	for (i = 0; i < 3; i++) {
		mmio_write(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		mmio_write(TCS(TCS_CMD_ADDR(i)), address[i]);
		mmio_write(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	mmio_write(TCS(TCS_WAIT_FOR_COMPLETION), 7);
	mmio_write(TCS(TCS_CMD_ENABLE), 7);
	mmio_write(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	mmio_write(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	mmio_write(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (i = 0; i < 2000 && (mmio_read(TCS(TCS_IRQ_STATUS)) & 1) != 1; i++) {
		touch_us_delay(100);
	}
	mmio_write(TCS(TCS_CONTROL), mmio_read(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	mmio_write(TCS(TCS_CONTROL), mmio_read(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	mmio_write(TCS(TCS_CMD_ENABLE), 0);
	mmio_write(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	mmio_write(TCS(TCS_IRQ_CLEAR), 1);
	mmio_write(TCS(TCS_IRQ_ENABLE), irq_enable);
	print("touch: L28 (AVDD) requested over RPMh, at %x\n", base);
	return 1;
}

/* ---- the sequence that starts the scanning ---- */
static int touch_reset_and_scan(void)
{
	uint8_t event[8], command[5];

	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_RESET, 0, T_OUTPUT, T_PULLUP, 1), 1);
	touch_tlmm->gpio_out(TOUCH_GPIO_CFG(TOUCH_PIN_RESET, 0, T_OUTPUT, T_PULLUP, 1), 0);
	touch_us_delay(10000);
	touch_tlmm->gpio_out(TOUCH_GPIO_CFG(TOUCH_PIN_RESET, 0, T_OUTPUT, T_PULLUP, 1), 1);
	touch_us_delay(50000);

	if (!touch_wait_event(EVT_READY, 2000, event)) {
		print("touch: no \"controller ready\"\n");
		return 0;
	}

	/* the interrupt enable register: the firmware leaves it readable and partly writable */
	touch_hw_write(0x20000029, 0x41);

	/* flush the FIFO, then scan in active mode */
	{
		static const uint8_t flush[3] = { FTM5_SYSTEM, 0x00, 0x01 };
		static const uint8_t scan[3] = { FTM5_SCAN_MODE, 0x00, 0x01 };

		touch_write(flush, sizeof(flush));
		touch_us_delay(10000);
		if (!touch_command(scan, sizeof(scan))) {
			print("touch: scan mode not confirmed\n");
			return 0;
		}
	}
	print("touch: scanning\n");
	return 1;
}

static void touch_init(void)
{
	uint8_t b[8];

	if (EFI_ERROR(BS->locate_protocol(&g_tlmm_protocol, NULL, (void **)&touch_tlmm)) || touch_tlmm == NULL) {
		print("touch: no TLMM protocol\n");
		return;
	}

	/* the I/O rail first: without it the bus has no pull-ups and the chip is unreachable */
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_VDDIO, 0, T_OUTPUT, T_PULLUP, 0), 1);
	touch_tlmm->gpio_out(TOUCH_GPIO_CFG(TOUCH_PIN_VDDIO, 0, T_OUTPUT, T_PULLUP, 0), 1);
	touch_us_delay(5000);

	/* the interrupt line, an input: the poll uses it to skip empty reads */
	touch_tlmm->config_gpio(TOUCH_GPIO_CFG(TOUCH_PIN_INT, 0, T_INPUT, T_PULLUP, 0), 1);

	/* AVDD, then let the chip boot with both supplies up */
	if (!touch_power_avdd()) {
		print("touch: no ldoa28 in the command database\n");
	}
	touch_us_delay(20000);

	if (!touch_reset_and_scan()) {
		return;
	}
	/* chip id, for the record */
	{
		static const uint8_t chip_id[5] = { FTM5_HW_REG, 0x20, 0x00, 0x00, 0x00 };

		if (touch_read(chip_id, sizeof(chip_id), b, 2)) {
			print("touch: chip id %x %x\n", b[0], b[1]);
		}
	}
	touch_shared.magic = TOUCH_MAGIC;
	touch_shared.seq = 0;
	touch_shared.count = 0;
	touch_shared.down = 0;
	touch_ready = 1;
}

#define TOUCH_X_MAX 1079
#define TOUCH_Y_MAX 2339
#define TOUCH_SAMPLES 64

static void touch_post(uint32_t type, uint32_t finger, uint32_t cx, uint32_t cy)
{
	uint32_t index = touch_shared.count % TOUCH_SAMPLES;
	struct touch_sample *sample = &touch_shared.samples[index];
	uint32_t x = cx > TOUCH_X_MAX ? TOUCH_X_MAX : cx;
	uint32_t y = cy > TOUCH_Y_MAX ? TOUCH_Y_MAX : cy;

	sample->type = type;
	sample->finger = finger;
	sample->x = (uint32_t)((uint64_t)x * 65535 / TOUCH_X_MAX);
	sample->y = (uint32_t)((uint64_t)y * 65535 / TOUCH_Y_MAX);
	touch_shared.count = touch_shared.count + 1;
	touch_shared.seq = touch_shared.seq + 1;
	/*
	 * The kernel reads this page through an uncached (write-combining) mapping: what this core has
	 * in its cache is invisible to it until the lines are written back. Without this the events
	 * arrive in a burst whenever some other memory traffic happens to evict them.
	 */
	flush_dcache((uint64_t)(uintptr_t)sample, (uint64_t)(uintptr_t)sample + sizeof(*sample));
	flush_dcache((uint64_t)(uintptr_t)&touch_shared, (uint64_t)(uintptr_t)&touch_shared + 16);

}

/* Polled from the loader's loop: the kernel runs on the other core, this one keeps the bus. */
static uint32_t touch_polls, touch_reads, touch_events_seen, touch_fails;
static uint64_t touch_last_report;

static void touch_poll(void)
{
	static int failures;
	uint64_t now = READ_SYSREG(CNTVCT_EL0);

	touch_polls++;
	/*
	 * The chip pulls the interrupt line low when it has events. A read of one event costs about a
	 * millisecond of this core -- the same core that serves the kernel's block I/O -- so empty
	 * polls must not touch the bus. Every 64th poll reads anyway, in case the line is not wired
	 * the way this assumes.
	 */
	if (touch_int_level() != 0 && (touch_polls & 63) != 0) {
		return;
	}

	if (now - touch_last_report >= READ_SYSREG(CNTFRQ_EL0) * 30) {
		touch_last_report = now;
		print("touch: %d polls, %d reads, %d events, %d fails\n", touch_polls, touch_reads, touch_events_seen, touch_fails);
	}
	static const uint8_t read_event = FTM5_READ_EVENT;
	uint8_t event[8];
	uint32_t finger, type, cx, cy;

	if (!touch_ready) {
		return;
	}

	for (int i = 0; i < 4; i++) {
		touch_reads++;
		if (!touch_read(&read_event, 1, event, 8)) {
			touch_fails++;
			if (++failures >= 100) {
				print("touch: 100 reads in a row failed\n");
				touch_ready = 0;
			}
			touch_recover();
			return;
		}
		failures = 0;
		if (event[0] == 0x00) {
			return;
		}
		touch_events_seen++;
		finger = event[1] >> 4;
		type = event[1] & 0xf;
		cx = (uint32_t)(event[3] & 0x0f) << 8 | event[2];
		cy = (uint32_t)event[4] << 4 | (event[3] >> 4);
		switch (event[0]) {
		case 0x13: /* enter */
			touch_post(TOUCH_DOWN, finger, cx, cy);
			touch_shared.down |= 1u << (finger & 31);
			break;
		case 0x23: /* motion */
			touch_post(TOUCH_MOVE, finger, cx, cy);
			break;
		case 0x33: /* leave */
			touch_post(TOUCH_UP, finger, cx, cy);
			touch_shared.down &= ~(1u << (finger & 31));
			break;
		case 0x03: { /* the chip reset itself: bring scanning back */
			static const uint8_t scan[3] = { FTM5_SCAN_MODE, 0x00, 0x01 };

			(void)touch_command(scan, sizeof(scan));
			break;
		}
		default:
			break;
		}
		(void)type;
	}
}
