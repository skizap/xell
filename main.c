#include <types.h>
#include <cache.h>
#include <vsprintf.h>
#include <string.h>
#include <network.h>
#include <processor.h>
#include <time.h>

extern void try_boot_cdrom(void);

#include "elf_abi.h"

static void putch(unsigned char c)
{
	while (!((*(volatile uint32_t*)0x80000200ea001018)&0x02000000));
	*(volatile uint32_t*)0x80000200ea001014 = (c << 24) & 0xFF000000;
}

static int kbhit(void)
{
	uint32_t status;
	
	do
		status = *(volatile uint32_t*)0x80000200ea001018;
	while (status & ~0x03000000);
	
	return !!(status & 0x01000000);
}

int getchar(void)
{
	while (!kbhit());
	return (*(volatile uint32_t*)0x80000200ea001010) >> 24;
}

int putchar(int c)
{
	if (c == '\n')
		putch('\r');
	putch(c);
	return 0;
}

void putstring(const char *c)
{
	while (*c)
		putchar(*c++);
}

int puts(const char *c)
{
	putstring(c);
	putstring("\n");
	return 0;
}

/* e_ident */
#define IS_ELF(ehdr) ((ehdr).e_ident[EI_MAG0] == ELFMAG0 && \
                      (ehdr).e_ident[EI_MAG1] == ELFMAG1 && \
                      (ehdr).e_ident[EI_MAG2] == ELFMAG2 && \
                      (ehdr).e_ident[EI_MAG3] == ELFMAG3)

unsigned long load_elf_image(void *addr)
{
	Elf32_Ehdr *ehdr;
	Elf32_Shdr *shdr;
	unsigned char *strtab = 0;
	int i;

	ehdr = (Elf32_Ehdr *) addr;
	
	shdr = (Elf32_Shdr *) (addr + ehdr->e_shoff + (ehdr->e_shstrndx * sizeof (Elf32_Shdr)));

	if (shdr->sh_type == SHT_STRTAB)
		strtab = (unsigned char *) (addr + shdr->sh_offset);

	for (i = 0; i < ehdr->e_shnum; ++i)
	{
		shdr = (Elf32_Shdr *) (addr + ehdr->e_shoff +
				(i * sizeof (Elf32_Shdr)));

		if (!(shdr->sh_flags & SHF_ALLOC) || shdr->sh_size == 0)
			continue;

		if (strtab) {
			printf("0x%08x 0x%08x, %sing %s...",
				(int) shdr->sh_addr,
				(int) shdr->sh_size,
				(shdr->sh_type == SHT_NOBITS) ?
					"Clear" : "Load",
				&strtab[shdr->sh_name]);
		}

		void *target = (void*)(((unsigned long)0x8000000000000000UL) | shdr->sh_addr);

		if (shdr->sh_type == SHT_NOBITS) {
			memset (target, 0, shdr->sh_size);
		} else {
			memcpy ((void *) target, 
				(unsigned char *) addr + shdr->sh_offset,
				shdr->sh_size);
		}
		flush_code (target, shdr->sh_size);
		puts("done");
	}
	
	return ehdr->e_entry;
}

extern void jump(unsigned long dtc, unsigned long kernel_base, unsigned long null, unsigned long reladdr, unsigned long hrmor);
extern void boot_tftp(const char *server, const char *file);

extern char bss_start[], bss_end[], dt_blob_start[];

volatile unsigned long secondary_hold_addr = 1;

void enet_quiesce(void);

void execute_elf_at(void *addr)
{
	printf(" * Loading ELF file...\n");
	void *entry = (void*)load_elf_image(addr);
	
	printf(" * Stop ethernet...\n");
	enet_quiesce();
	printf(" * GO (entrypoint: %p)\n", entry);

	secondary_hold_addr = ((long)entry) | 0x8000000000000060UL;
	
	jump(((long)dt_blob_start)&0x7fffffffffffffffULL, (long)entry, 0, (long)entry, 0);
}

volatile int processors_online[6] = {1};

int get_online_processors(void)
{
	int i;
	int res = 0;
	for (i=0; i<6; ++i)
		if (processors_online[i])
			res |= 1<<i;
	return res;
}

void place_jump(void *addr, void *_target)
{
	unsigned long target = (unsigned long)_target;
	dcache_flush(addr - 0x80, 0x100);
	*(volatile uint32_t*)(addr - 0x18 + 0) = 0x3c600000 | ((target >> 48) & 0xFFFF);
	*(volatile uint32_t*)(addr - 0x18 + 4) = 0x786307c6;
	*(volatile uint32_t*)(addr - 0x18 + 8) = 0x64630000 | ((target >> 16) & 0xFFFF);
	*(volatile uint32_t*)(addr - 0x18 + 0xc) = 0x60630000 | (target & 0xFFFF);
	*(volatile uint32_t*)(addr - 0x18 + 0x10) = 0x7c6803a6;
	*(volatile uint32_t*)(addr - 0x18 + 0x14) = 0x4e800021;
	flush_code(addr-0x18, 0x18);
	*(volatile uint32_t*)(addr + 0) = 0x4bffffe8;
	flush_code(addr, 0x80);
}

extern char __start_other[], __exception[];

#define HRMOR (0x10000000000ULL)

void syscall();

int start(int pir, unsigned long hrmor, unsigned long pvr, void *r31)
{
	secondary_hold_addr = 0;

	int exc[]={0x100, 0x200, 0x300, 0x380, 0x400, 0x480, 0x500, 0x600, 0x700, 0x800, 0x900, 0x980, 0xC00, 0xD00, 0xF00, 0xF20, 0x1300, 0x1600, 0x1700, 0x1800};

	int i;
	
	printf("\nXeLL - Xenon linux loader 0.1\n");

	printf(" * clearing BSS...\n");
		/* clear BSS, we're already late */
	unsigned char *p = (unsigned char*)bss_start;
	memset(p, 0, bss_end - bss_start);

	printf(" * Attempting to catch all CPUs...\n");

	for (i=0; i<sizeof(exc)/sizeof(*exc); ++i)
		place_jump((void*)HRMOR + exc[i], __start_other);

	place_jump((void*)0x8000000000000700, __start_other);
	
	while (get_online_processors() != 0x3f)
	{
		printf("CPUs online: %02x..\r", get_online_processors()); mdelay(1000);
		if ((get_online_processors() & 0x15) == 0x15)
		{
			for (i=0; i<sizeof(exc)/sizeof(*exc); ++i)
				place_jump((void*)0x8000000000000000ULL + exc[i], __start_other);
			
			for (i=1; i<6; ++i)
			{
				*(volatile uint64_t*)(0x8000020000050070ULL + i * 0x1000) = 0x7c;
				*(volatile uint64_t*)(0x8000020000050068ULL + i * 0x1000) = 0;
				(void)*(volatile uint64_t*)(0x8000020000050008ULL + i * 0x1000);
				while (*(volatile uint64_t*)(0x8000020000050050ULL + i * 0x1000) != 0x7C);
			}
			
			*(uint64_t*)(0x8000020000052010ULL) = 0x3e0078;
		}
	}
	
	printf(" * success.\n");

			/* re-reset interrupt controllers. especially, remove their pending IPI IRQs. */
	for (i=1; i<6; ++i)
	{
		*(uint64_t*)(0x8000020000050068ULL + i * 0x1000) = 0x74;
		while (*(volatile uint64_t*)(0x8000020000050050ULL + i * 0x1000) != 0x7C);
	}

		/* remove any characters left from bootup */
	while (kbhit())
		getchar();
	
	network_init();

		/* display some cpu info */
	printf(" * CPU PVR: %08lx\n", mfspr(287));

	printf(" * FUSES - write them down and keep them safe:\n");
	for (i=0; i<12; ++i)
		printf("fuseset %02d: %016lx\n", i, *(unsigned long*)(0x8000020000020000 + (i * 0x200)));

	if (get_online_processors() != 0x3f)
		printf("WARNING: not all processors could be woken up.\n");

	printf(" * try booting from CDROM\n");
	try_boot_cdrom();
	printf(" * try booting tftp\n");
	boot_tftp("10.0.0.1", "/tftpboot/xenon");
	printf(" * HTTP listen\n");
	while (1) network_poll();

	return 0;
}
