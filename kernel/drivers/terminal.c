#include <limine.h>
#include <mm/vmm.h>
#include <string.h>
#include <sched/sched.h>
#include <int/apic.h>
#include <int/idt.h>
#include <drivers/terminal.h>
#include <debug.h>

typeof(terminal_list) terminal_list;
struct terminal *current_terminal;

static volatile struct limine_terminal_request limine_terminal_request = {
	.id = LIMINE_TERMINAL_REQUEST,
	.revision = 0
};

static volatile struct limine_framebuffer_request limine_framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

static struct page_table terminal_page_table;

void terminal_stream_push(struct terminal *terminal, char c) {
	if(terminal->stream_capacity <= terminal->stream_index) {
		terminal->stream_capacity += 0x1000;
		terminal->stream = realloc(terminal->stream, terminal->stream_capacity);
	}

	terminal->stream[terminal->stream_index++] = c;
}

void ps2_handler(struct registers*, void*) {
	static char keymap[] = {
		'\0', '\0', '1', '2', '3',	'4', '5', '6',	'7', '8', '9', '0',
		'-', '=', '\b', '\t', 'q',	'w', 'e', 'r',	't', 'y', 'u', 'i',
		'o', 'p', '[', ']', '\n',  '\0', 'a', 's',	'd', 'f', 'g', 'h',
		'j', 'k', 'l', ';', '\'', '`', '\0', '\\', 'z', 'x', 'c', 'v',
		'b', 'n', 'm', ',', '.',  '/', '\0', '\0', '\0', ' '
	};

	static char cap_keymap[] = {
		'\0', '\e', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
		'_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
		'O', 'P', '{', '}', '\0', '\0', 'A', 'S', 'D', 'F', 'G', 'H',
		'J', 'K', 'L', ':', '\'', '~', '\0', '\\', 'Z', 'X', 'C', 'V',
		'B', 'N', 'M', '<', '>',  '?', '\0', '\0', '\0', ' '
	};
	
	uint8_t keycode = inb(0x60);
	char character = '\0';

	switch(keycode) {
		case 0xaa:
			current_terminal->shift = true;
			return;
		case 0x2a:
			current_terminal->shift = false;
			return;
		case 0x36:
			current_terminal->shift = true;
			return;
		case 0xb6:
			current_terminal->shift = false;
			return;
		case 0x3a:
			current_terminal->caplock = !current_terminal->caplock;
			return;
		case 0x1d:
			current_terminal->ctrl = true;
			return;
		default:
			if(keycode <= 128) {
				int caps = current_terminal->caplock ^ current_terminal->shift;
				if(caps) {
					character = cap_keymap[keycode];
				} else {
					character = keymap[keycode];
				}
				terminal_stream_push(current_terminal, character);
			}
	}
}

ssize_t terminal_read(struct asset*, void*, off_t, off_t, void *buffer) {
	volatile struct terminal *terminal = (volatile struct terminal*)current_terminal;

	while(!terminal->stream_index);

	for(size_t i = 0; i < terminal->stream_index; i++) {
		*(char*)(buffer + i) = terminal->stream[i];
	}
	
	limine_terminal_print(terminal->stream, terminal->stream_index);

	ssize_t read = terminal->stream_index;
	terminal->stream_index = 0;

	return read;
}

ssize_t terminal_write(struct asset*, void*, off_t, off_t cnt, const void *buffer) {
	limine_terminal_print((void*)buffer, cnt);
	return cnt;
}

void limine_terminal_init() {
	vmm_default_table(&terminal_page_table);

	struct limine_framebuffer *framebuffer = limine_framebuffer_request.response->framebuffers[0];

	uint64_t phys = 0;
	for(size_t i = 0; i < 0x800; i++) {
		terminal_page_table.map_page(&terminal_page_table, phys, phys,	VMM_FLAGS_P |
																		VMM_FLAGS_RW |
																		VMM_FLAGS_PS |
																		VMM_FLAGS_G);
		phys += 0x200000;
	}

	uint64_t fbaddr = (uintptr_t)framebuffer->address - HIGH_VMA;
	uint64_t fbsize = (framebuffer->width * framebuffer->bpp * framebuffer->pitch) / 8;

	for(size_t i = 0; i < DIV_ROUNDUP(fbsize, 0x200000); i++) {
		terminal_page_table.map_page(&terminal_page_table, fbaddr, fbaddr,	VMM_FLAGS_P |
																			VMM_FLAGS_RW |
																			VMM_FLAGS_PS |
																			VMM_FLAGS_G);
		fbaddr += 0x200000;
	}

	struct limine_terminal **limine_terminals = limine_terminal_request.response->terminals;
	uint64_t term_cnt = limine_terminal_request.response->terminal_count;

	for(size_t i = 0; i < term_cnt; i++) {
		struct terminal *terminal = alloc(sizeof(struct terminal));

		terminal->limine_terminal = limine_terminals[i];
		terminal->stream = alloc(0x1000);
		terminal->stream_capacity = 0x1000; 

		VECTOR_PUSH(terminal_list, terminal);
	}

	current_terminal = terminal_list.data[0];

	int ps2_vector = idt_alloc_vector(ps2_handler, NULL);
	ioapic_set_irq_redirection(xapic_read(XAPIC_ID_REG_OFF), ps2_vector, 1, false);
}

void limine_terminal_print(char *str, size_t length) {
	asm volatile ("cli");
	spinlock(&current_terminal->lock);

	char *data = alloc(length);
	memcpy8((void*)data, (void*)str, length);

	vmm_init_page_table(&terminal_page_table);

	struct limine_terminal *terminal = limine_terminal_request.response->terminals[0];
	limine_terminal_request.response->write(terminal, data, length);

	vmm_init_page_table(CORE_LOCAL->page_table);

	spinrelease(&current_terminal->lock);
	asm volatile ("sti");
}
