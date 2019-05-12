#define _GNU_SOURCE 1

#ifndef __x86_64__
#error Unsupported ARCH :(
#endif

#include "hook_engine.h"
#include "hde/hde64.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <dlfcn.h>

struct Overlay
{
    uintptr_t target; // Where in address space this will ultimately end up.
    uint8_t  *p;      // Current output position.
    uint8_t   code[HOOK_TRAMPOLINE_LEN];
};

static __thread int g_mem_fd = -1;
static __thread char g_errmsg[256];

#define FORMAT_ERRMSG(...) snprintf(g_errmsg, sizeof g_errmsg, __VA_ARGS__)

// Dump as many bytes as fit into buf, but don't cross page boundary.
static
const char *hexdump(const void *mem, char *buf, size_t size)
{
    uintptr_t page_mask    = sysconf(_SC_PAGESIZE) - 1;
    const uint8_t *mem_p   = mem;
    const uint8_t *mem_end = (const uint8_t *)(((uintptr_t)mem + page_mask) & ~page_mask);

    char *buf_p            = buf;
    char *buf_end          = buf + size;

    while (mem_p < mem_end && buf_p + 3 < buf_end)
        buf_p += sprintf(buf_p, "%02x", *mem_p++);

    return buf;
}

static
const char *funcname(void *func)
{
    Dl_info info;

    if (dladdr(func, &info) == 0 || !info.dli_sname)
        return "Unknown";

    return info.dli_sname;
}

static inline
void put_uint32(uint8_t *p, uint32_t v)
{
    struct u32 { uint32_t u32; } __attribute__((__packed__));
    ((struct u32*)(p))->u32 = v;
}

static inline
void put_uint64(uint8_t *p, uint64_t v)
{
    struct u64 { uint64_t u64; } __attribute__((__packed__));
    ((struct u64*)(p))->u64 = v;
}

static inline
size_t overlay_size(const struct Overlay *c)
{
    return c->p - c->code;
}

static
int install_overlay(const struct Overlay *c)
{
    ssize_t c_size = overlay_size(c);

    if (g_mem_fd == -1) {
        // No /proc/self/mem available, use mprotect.

        uintptr_t page_mask  = sysconf(_SC_PAGESIZE) - 1;
        uintptr_t page_begin = c->target & ~page_mask;
        uintptr_t page_end   = (c->target + c_size + page_mask) & ~page_mask;

        if (mprotect((void*)page_begin, page_end - page_begin, PROT_WRITE|PROT_EXEC) != 0)
            goto error;

        memcpy((void*)c->target, c->code, c_size);

        mprotect((void*)page_begin, page_end - page_begin, PROT_EXEC);
        return 0;
    }

    if (pwrite(g_mem_fd, c->code, c_size, c->target) == c_size) {
        return 0;
    }

error:
    FORMAT_ERRMSG("patching code in memory: %s", strerror(errno));
    return -1;
}

static
void write_initial_jmp(struct Overlay *c, uintptr_t target)
{
    // movq target, %rax
    c->p[0] = 0x48;
    c->p[1] = 0xB8;
    put_uint64(c->p + 2, target);

    // jmp %rax
    c->p[10] = 0xFF;
    c->p[11] = 0xE0;

    c->p += 12;
}

static
void write_jmp(struct Overlay *c, uintptr_t target, uintptr_t **jump_table)
{
    // jmp  *jump_table(%rip)
    c->p[0] = 0xff;
    c->p[1] = 0x25;
    put_uint32(c->p + 2, (uintptr_t)(*jump_table) - (c->target + overlay_size(c) + 6));

    c->p += 6;
    (*jump_table)[0] = target;
    (*jump_table)++;
}

static
void write_call(struct Overlay *c, uintptr_t target, uintptr_t **jump_table)
{
    // call *jump_table(%rip)
    write_jmp(c, target, jump_table);
    c->p[-5] = 0x15;
}

// used by get_jump_table() for validation
void reference_trampoline(void);
HOOK_DEFINE_TRAMPOLINE(reference_trampoline);

static
uint64_t *get_jump_table(void *trampoline)
{
    // validate trampoline - must be in a clean state,
    // HOOK_TRAMPOLINE_LEN * (int 3) instructions,
    // followed by lea [rip-relative], %rax.
    // +3 in memcmp below to check lea instruction (minus offset part).
    if (memcmp(trampoline, (void *)reference_trampoline,
               HOOK_TRAMPOLINE_LEN + 3) != 0) {
        return NULL;
    }

    // trampoline points to a symbol created with HOOK_DEFINE_TRAMPOLINE
    // macro; after TRAMPOLINE_LEN area, there's a code sequence
    // returning a pointer to a per-trampoline jump table
    return ((uint64_t*(*)(void)) ((uintptr_t)trampoline + HOOK_TRAMPOLINE_LEN)) ();
}

// Patch function @fn, so that every time it is called, control is
// transferred to @replacement.
//
// If @trampoline is provided, instructions destroyed in @fn are
// transferred to @trampoline.
int hook_install(void *fn, void *replacement, void *trampoline)
{
    // We render code in two overlays, and later overwrite @fn and
    // @trampoline with the overlays' content.
    struct Overlay fn_overlay = {(uintptr_t)fn, fn_overlay.code};
    struct Overlay t_overlay  = {(uintptr_t)trampoline, t_overlay.code};

    // Jump table is normally extracted from the trampoline. Provide a
    // placeholder if trampoline is NULL
    uint64_t jump_table_data[HOOK_JUMP_MAX], *jump_table = jump_table_data;

    if (trampoline && !(jump_table = get_jump_table(trampoline))) {
        FORMAT_ERRMSG("bad trampoline pointer");
        return -1;
    }

    // Prepare code to overwrite @fn with. This will be JMP @replacement.
    write_initial_jmp(&fn_overlay, (uintptr_t)replacement);

    // @fn is going to be partially clobbered. Disassemble and evacuate
    // some instructions.
    uintptr_t rip = (uintptr_t)fn;
    const uintptr_t rip_hazzard = rip + overlay_size(&fn_overlay);

    while (rip < rip_hazzard) {

        hde64s    s;
        uintptr_t rip_dest = UINTPTR_MAX;
        char      hexdump_buf[HOOK_CLOBBERED_LEN*2 + 1];

        hde64_disasm((const uint8_t *)rip, &s);
        if (s.flags & F_ERROR) {
            FORMAT_ERRMSG("creating trampoline for %s: unknown instruction at offset %+d (%s)",
                          funcname(fn),
                          (int)(rip - (uintptr_t)fn),
                          hexdump(fn, hexdump_buf, sizeof hexdump_buf));
            return -1;
        }

        rip += s.len;

        switch (s.opcode) {

        case 0xCC:
            FORMAT_ERRMSG("creating trampoline for %s: 'INT 3' instruction, "
                          "looks like a breakpoint set by debugger (%s)",
                          funcname(fn),
                          hexdump(fn, hexdump_buf, sizeof hexdump_buf));
            return -1;

        case 0xE8:
            // relative call, 32 bit immediate offset
            assert(s.flags & F_IMM32);
            rip_dest = rip + (int32_t)s.imm.imm32;
            write_call(&t_overlay, rip_dest, &jump_table);
            goto check_rip_dest;

        case 0xE9:
            // relative jump, 8 bit immediate offset
            assert(s.flags & F_IMM8);
            rip_dest = rip + (int8_t)s.imm.imm8;
            write_jmp(&t_overlay, rip_dest, &jump_table);
            goto check_rip_dest;

        case 0xEB:
            // relative jump, 32 bit immediate offset
            assert(s.flags & F_IMM32);
            rip_dest = rip + (int32_t)s.imm.imm32;
            write_jmp(&t_overlay, rip_dest, &jump_table);
            goto check_rip_dest;

        case 0xE3:
            // jump if %ecx/ %rcx zero
            FORMAT_ERRMSG("creating trampoline for %s: 'JCXZ' instruction not supported (%s)",
                          funcname(fn),
                          hexdump(fn, hexdump_buf, sizeof hexdump_buf));
            return -1;

        case 0x70 ... 0x7f:
            // Jcc jump, 8 bit immediate offset
            assert(s.flags & F_IMM8);
            rip_dest = rip + (int8_t)s.imm.imm8;

            t_overlay.p[0] = s.opcode ^ 1;
            t_overlay.p[1] = 6;
            t_overlay.p += 2;

            write_jmp(&t_overlay, rip_dest, &jump_table);
            goto check_rip_dest;

        case 0x0F:
            if (s.opcode2 >= 0x80 && s.opcode2 <= 0x8F) {
                // Jcc jump, 32 bit immediate offset
                assert(s.flags & F_IMM32);
                rip_dest = rip + (int32_t)s.imm.imm32;

                // Convert to a shorter form
                t_overlay.p[0] = (s.opcode2 - 0x10) ^ 1;
                t_overlay.p[1] = 6;
                t_overlay.p += 2;

                write_jmp(&t_overlay, rip_dest, &jump_table);
                goto check_rip_dest;
            }
            break;
        }

        // RIP-relative addressing
        if ((s.flags & F_MODRM) &&
            s.modrm_mod == 0 && s.modrm_rm == 0x5) {

            // LEA?
            if (s.opcode != 0x8D) {
                FORMAT_ERRMSG("creating trampoline for %s: %%rip-relative addressing (%s)",
                              funcname(fn),
                              hexdump(fn, hexdump_buf, sizeof hexdump_buf));
                return -1;
            }

            // Convert to MOV
            t_overlay.p[0] = 0x48 + s.rex_r;
            t_overlay.p[1] = 0xB8 + s.modrm_reg;
            put_uint64(t_overlay.p + 2, rip + (int32_t)s.disp.disp32);
            t_overlay.p += 10;

            continue;
        }

        // Copy instruction
        memcpy(t_overlay.p, (const uint8_t *)rip - s.len, s.len);
        t_overlay.p += s.len;
        continue;

check_rip_dest:
        // If we've seen a jump into the range we are about to overwrite,
        // this isn't going to work.
        if (rip_dest > (uintptr_t)fn && rip_dest < rip_hazzard) {
            FORMAT_ERRMSG("creating trampoline for %s: a jump into the "
                          "clobbered instruction range encountered (%s)",
                          funcname(fn),
                          hexdump(fn, hexdump_buf, sizeof hexdump_buf));
            return -1;
        }
    }

    // If we've clobbered a *part* of an instruction, we should beter
    // int3 the surviving part.
    size_t partially_clobbered = rip - rip_hazzard;
    memset(fn_overlay.p, 0xcc, partially_clobbered);
    fn_overlay.p += partially_clobbered;

    // Connect trampoline to the unclobbered part of @fn.
    write_jmp(&t_overlay, rip, &jump_table);

    // Now actually owerwrite things.
    if (trampoline && install_overlay(&t_overlay) != 0)
        return -1;

    return install_overlay(&fn_overlay);
}

int hook_begin()
{
    if (g_mem_fd == -1) {
        g_mem_fd = open("/proc/self/mem", O_WRONLY);
    }

    return 0;
}

void hook_end()
{
    if (g_mem_fd != -1)
        close(g_mem_fd);

    g_mem_fd = - 1;
}

const char *hook_last_error()
{
    return g_errmsg;
}
