/*
 * The other half of the touch port: the kernel reads the page the loader fills.
 *
 * XNU must not touch this phone's PMIC, TLMM or QUP (an RPMh write resets the machine), so the
 * ST FTS is driven from UEFI and its samples land in a page the loader names on the command line
 * as touch_shared_pa=0x鈥? This file does nothing but read that page and post the events to
 * /dev/redwood-touch, which is the same path the goodix driver uses.
 */

#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/redwood_input.h>

#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);

#define TOUCH_MAGIC   0x43554f54u /* 'TOUC' */
#define TOUCH_SAMPLES 64

struct touch_shared_sample {
	uint32_t type, finger, x, y;
};

struct touch_shared_page {
	volatile uint32_t magic;
	volatile uint32_t seq;
	volatile uint32_t count;
	volatile uint32_t down;
	struct touch_shared_sample samples[TOUCH_SAMPLES];
};

static struct touch_shared_page *touch_page;

static void
perseus_touch_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t consumed = 0;
	uint32_t idle = 0;
	uint32_t down = 0;

	(void)parameter;
	for (;;) {
		uint32_t seq, count, i;

		if (touch_page->magic != TOUCH_MAGIC) {
			delay(100);
			continue;
		}
		seq = touch_page->seq;
		count = touch_page->count;
		if (seq != 0 && count != consumed) {
			uint32_t first = consumed;

			/* a reader that falls behind skips what the ring no longer holds */
			if (count - first > TOUCH_SAMPLES) {
				first = count - TOUCH_SAMPLES;
			}
			for (i = first; i != count; i++) {
				struct touch_shared_sample *s = &touch_page->samples[i % TOUCH_SAMPLES];
				uint32_t type = s->type, finger = s->finger, x = s->x, y = s->y;

				if (type < REDWOOD_TOUCH_DOWN || type > REDWOOD_TOUCH_UP || finger >= 32) {
					continue;
				}
				redwood_touch_post(type, finger, x, y);
			}
			consumed = count;
			idle = 0;
		} else if (++idle % 200 == 0 && touch_page->down == 0) {
			/* here so a stuck page shows up in the log rather than as silence */
			printf("perseus-touch: %u samples so far, none new\n", consumed);
		}
		delay(2);
	}
}

void
perseus_touch_init(void)
{
	uint64_t address = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touch_shared_pa", &address, sizeof(address))) {
		return;
	}
	if (address == 0) {
		return;
	}
	/* a physical address is not a kernel pointer: the page has to be mapped (as redwood_ufs.c does) */
	{
		vm_offset_t base = ml_io_map_unmappable((vm_offset_t)(address & ~0xfffULL),
		    (vm_size_t)(((address & 0xfff) + sizeof(struct touch_shared_page) + 0xfff) & ~0xfffULL), VM_WIMG_WCOMB);

		touch_page = (struct touch_shared_page *)(base + (address & 0xfff));
	}
	if (kernel_thread_start(perseus_touch_thread, NULL, &thread) == KERN_SUCCESS) {
		printf("perseus-touch: reading the loader's page at %#llx (magic %#x)\n", address, touch_page->magic);
		thread_deallocate(thread);
	}
}
