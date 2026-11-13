#include "patchs/oplus/sku.h"

#include "arm64_inst/arm64_inst_decoder.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PE_MACHINE_ARM64 0xAA64u
#define PE_SECTION_EXECUTABLE 0x20000000u
#define MAX_PE_SECTIONS 96u
#define TRAMPOLINE_WORDS 14u
#define TRAMPOLINE_BYTES (TRAMPOLINE_WORDS * 4u)
#define MIN_CAVE_RUN 0x80u

typedef struct {
    uint32_t rva;
    uint32_t raw;
    uint32_t raw_size;
    uint32_t virtual_size;
    uint32_t flags;
} PeSection;

typedef struct {
    PeSection sections[MAX_PE_SECTIONS];
    uint16_t count;
} PeImage;

static bool range_ok(int32_t size, uint32_t off, uint32_t len) {
    return size > 0 && off <= (uint32_t)size && len <= (uint32_t)size - off;
}

static bool read_u16(const char* data, int32_t size, uint32_t off, uint16_t* value) {
    if (!range_ok(size, off, 2)) return false;
    *value = (uint16_t)(uint8_t)data[off]
           | (uint16_t)((uint16_t)(uint8_t)data[off + 1] << 8);
    return true;
}

static bool read_u32(const char* data, int32_t size, uint32_t off, uint32_t* value) {
    if (!range_ok(size, off, 4)) return false;
    *value = read_instr(data, (int32_t)off);
    return true;
}

static bool parse_pe(const char* data, int32_t size, PeImage* image) {
    uint32_t pe;
    uint16_t machine, count, optional_size;
    uint32_t table;

    memset(image, 0, sizeof(*image));
    if (!range_ok(size, 0, 0x40) || data[0] != 'M' || data[1] != 'Z') return false;
    if (!read_u32(data, size, 0x3c, &pe) || pe < 0x40 ||
        !range_ok(size, pe, 24) || memcmp(data + pe, "PE\0\0", 4) != 0) return false;
    if (!read_u16(data, size, pe + 4, &machine) || machine != PE_MACHINE_ARM64 ||
        !read_u16(data, size, pe + 6, &count) || count == 0 || count > MAX_PE_SECTIONS ||
        !read_u16(data, size, pe + 20, &optional_size)) return false;

    table = pe + 24u + optional_size;
    if (!range_ok(size, table, (uint32_t)count * 40u)) return false;
    image->count = count;
    for (uint16_t index = 0; index < count; ++index) {
        uint32_t off = table + (uint32_t)index * 40u;
        PeSection* section = &image->sections[index];
        if (!read_u32(data, size, off + 8, &section->virtual_size) ||
            !read_u32(data, size, off + 12, &section->rva) ||
            !read_u32(data, size, off + 16, &section->raw_size) ||
            !read_u32(data, size, off + 20, &section->raw) ||
            !read_u32(data, size, off + 36, &section->flags) ||
            !range_ok(size, section->raw, section->raw_size)) return false;
    }
    return true;
}


/* This patch changes only the bootconfig formatting argument, not the shared
 * project classifier (which also produces oplusboot.sku). */
#define AND_SKU_W3_MASKED 0x12001c03u
#define PROJECT_TEXT " androidboot.prjname="
#define SKU_TEXT " androidboot.product.hardware.sku=%d"

static bool raw_to_rva(const PeImage* pe, uint32_t raw, uint32_t length,
                       uint32_t* rva, uint32_t* flags) {
    for (uint16_t i = 0; i < pe->count; ++i) {
        const PeSection* s = &pe->sections[i];
        if (raw >= s->raw && raw - s->raw <= s->raw_size &&
            length <= s->raw_size - (raw - s->raw)) {
            *rva = s->rva + raw - s->raw;
            if (flags) *flags = s->flags;
            return true;
        }
    }
    return false;
}

static bool rva_to_raw(const PeImage* pe, uint32_t rva, uint32_t length,
                       uint32_t* raw, uint32_t* flags) {
    for (uint16_t i = 0; i < pe->count; ++i) {
        const PeSection* s = &pe->sections[i];
        if (rva >= s->rva && rva - s->rva <= s->raw_size &&
            length <= s->raw_size - (rva - s->rva)) {
            *raw = s->raw + rva - s->rva;
            if (flags) *flags = s->flags;
            return true;
        }
    }
    return false;
}

static bool unique_string(const char* data, int32_t size, const PeImage* pe,
                          const char* value, uint32_t* rva) {
    size_t n = strlen(value) + 1;
    unsigned count = 0;
    for (uint32_t off = 0; off + n <= (uint32_t)size; ++off) {
        uint32_t found;
        if (memcmp(data + off, value, n) != 0) continue;
        if (!raw_to_rva(pe, off, (uint32_t)n, &found, NULL)) continue;
        *rva = found;
        ++count;
    }
    return count == 1;
}

static bool decode_adrp(uint32_t word, uint32_t pc, uint8_t reg,
                        uint32_t* page) {
    int32_t imm;
    int64_t result;
    if ((word & 0x9f00001fu) != (0x90000000u | reg)) return false;
    imm = (int32_t)((((word >> 5) & 0x7ffffu) << 2) | ((word >> 29) & 3u));
    if (imm & 0x100000) imm -= 0x200000;
    result = (int64_t)(pc & ~0xfffu) + (int64_t)imm * 4096;
    if (result < 0 || result > UINT32_MAX) return false;
    *page = (uint32_t)result;
    return true;
}

static bool decode_add_x_imm(uint32_t word, uint8_t reg, uint32_t* imm) {
    if ((word & 0xffc003ffu) != (0x91000000u | ((uint32_t)reg << 5) | reg))
        return false;
    *imm = (word >> 10) & 0xfffu;
    return true;
}

static bool decode_mov_x0(uint32_t word, uint8_t* source) {
    if ((word & ~0x001f0000u) != 0xaa0003e0u) return false;
    *source = (uint8_t)((word >> 16) & 31u);
    return *source != 31u;
}

static bool decode_movz_w1_size(uint32_t word) {
    if ((word & 0xff80001fu) != 0x52800001u ||
        ((word >> 21) & 3u) != 0) return false;
    uint32_t size = (word >> 5) & 0xffffu;
    return size >= 0x20u && size <= 0x400u;
}

static bool is_and_w3_low8(uint32_t word) {
    return (word & ~0x000003e0u) == AND_SKU_W3_MASKED &&
           ((word >> 5) & 31u) != 31u;
}

static bool unique_x2_ref(const char* data, int32_t size, const PeImage* pe,
                          uint32_t target, uint32_t* raw, uint32_t* rva) {
    unsigned count = 0;
    for (uint16_t i = 0; i < pe->count; ++i) {
        const PeSection* s = &pe->sections[i];
        uint32_t usable = s->virtual_size && s->virtual_size < s->raw_size ?
                          s->virtual_size : s->raw_size;
        if (!(s->flags & PE_SECTION_EXECUTABLE)) continue;
        for (uint32_t delta = 0; delta + 8 <= usable; delta += 4) {
            uint32_t page, immediate, off = s->raw + delta;
            uint32_t pc = s->rva + delta;
            if (!range_ok(size, off, 8) ||
                !decode_adrp(read_instr(data, (int32_t)off), pc, 2, &page) ||
                !decode_add_x_imm(read_instr(data, (int32_t)off + 4), 2,
                                  &immediate) || page + immediate != target) continue;
            *raw = off + 4;
            *rva = pc + 4;
            ++count;
        }
    }
    return count == 1;
}

static bool encode_b(uint32_t src, uint32_t dst, uint32_t* out) {
    int64_t d = (int64_t)dst - src;
    if ((d & 3) || d < -(1ll << 27) || d >= (1ll << 27)) return false;
    *out = 0x14000000u | ((uint32_t)(d / 4) & 0x03ffffffu);
    return true;
}

static bool encode_adr(uint32_t src, uint32_t dst, uint8_t reg, uint32_t* out) {
    int64_t d = (int64_t)dst - src;
    uint32_t imm;
    if (d < -(1ll << 20) || d >= (1ll << 20)) return false;
    imm = (uint32_t)d & 0x1fffffu;
    *out = 0x10000000u | ((imm & 3u) << 29) |
           (((imm >> 2) & 0x7ffffu) << 5) | reg;
    return true;
}

static bool relocation_hits(const char* data, int32_t size, const PeImage* pe,
                            uint32_t cave, uint32_t length) {
    uint32_t header, opt, reloc_rva, reloc_size, reloc_raw;
    uint16_t magic, optional_size;
    if (!read_u32(data, size, 0x3c, &header) ||
        !read_u16(data, size, header + 20u, &optional_size)) return true;
    opt = header + 24u;
    if (optional_size < 160u ||
        !read_u16(data, size, opt, &magic) || magic != 0x20bu ||
        !read_u32(data, size, opt + 152u, &reloc_rva) ||
        !read_u32(data, size, opt + 156u, &reloc_size)) return true;
    if (reloc_rva == 0 && reloc_size == 0) return false;
    if (reloc_size < 8 ||
        !rva_to_raw(pe, reloc_rva, reloc_size, &reloc_raw, NULL)) return true;
    uint32_t pos = reloc_raw, end = reloc_raw + reloc_size;
    while (pos + 8u <= end) {
        uint32_t page, block;
        if (!read_u32(data, size, pos, &page) ||
            !read_u32(data, size, pos + 4u, &block)) return true;
        if (!page && !block) return false; /* PE directory padding */
        if (block < 8u || (block & 1u) || block > end - pos) return true;
        for (uint32_t off = pos + 8u; off < pos + block; off += 2u) {
            uint16_t entry;
            if (!read_u16(data, size, off, &entry)) return true;
            uint32_t target = page + (entry & 0xfffu);
            if ((entry >> 12) != 0 && target < cave + length &&
                target + 8u > cave) return true;
        }
        pos += block;
    }
    return pos != end;
}

static bool directory_hits(const char* data, int32_t size, uint32_t cave,
                           uint32_t length) {
    uint32_t header, opt, count;
    uint16_t magic, optional_size;
    if (!read_u32(data, size, 0x3c, &header) ||
        !read_u16(data, size, header + 20u, &optional_size)) return true;
    opt = header + 24u;
    if (optional_size < 112u ||
        !read_u16(data, size, opt, &magic) || magic != 0x20bu ||
        !read_u32(data, size, opt + 108u, &count)) return true;
    uint32_t available = (optional_size - 112u) / 8u;
    if (count > available) return true;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rva, bytes;
        if (!read_u32(data, size, opt + 112u + i * 8u, &rva) ||
            !read_u32(data, size, opt + 116u + i * 8u, &bytes)) return true;
        if (rva && bytes && (uint64_t)rva < (uint64_t)cave + length &&
            (uint64_t)rva + bytes > cave) return true;
    }
    return false;
}

static bool branch_targets_cave(const char* data, const PeSection* section,
                                uint32_t usable, uint32_t cave) {
    for (uint32_t delta = 0; delta + 4u <= usable; delta += 4u) {
        uint32_t pc = section->rva + delta;
        uint32_t word = read_instr(data, (int32_t)(section->raw + delta));
        int32_t units;
        if ((word & 0x7c000000u) == 0x14000000u) {
            units = (int32_t)(word & 0x03ffffffu);
            if (units & 0x02000000) units -= 0x04000000;
        } else if ((word & 0xff000010u) == 0x54000000u ||
                   (word & 0x7e000000u) == 0x34000000u) {
            units = (int32_t)((word >> 5) & 0x7ffffu);
            if (units & 0x40000) units -= 0x80000;
        } else if ((word & 0x7e000000u) == 0x36000000u) {
            units = (int32_t)((word >> 5) & 0x3fffu);
            if (units & 0x2000) units -= 0x4000;
        } else continue;
        int64_t target = (int64_t)pc + (int64_t)units * 4;
        if (target >= cave && target < (int64_t)cave + TRAMPOLINE_BYTES)
            return true;
    }
    return false;
}

static bool choose_cave(const char* data, int32_t size, const PeImage* pe,
                        uint32_t hook, uint32_t project,
                        uint32_t* raw, uint32_t* rva) {
    for (uint16_t i = 0; i < pe->count; ++i) {
        const PeSection* s = &pe->sections[i];
        uint32_t usable = s->virtual_size && s->virtual_size < s->raw_size ?
                          s->virtual_size : s->raw_size;
        uint32_t branch, adr;
        if (!(s->flags & PE_SECTION_EXECUTABLE) ||
            hook < s->rva || hook >= s->rva + usable ||
            usable < MIN_CAVE_RUN) continue;
        uint32_t tail = usable;
        while (tail && data[s->raw + tail - 1u] == 0) --tail;
        uint32_t start = (tail + 3u) & ~3u;
        if (usable - start < MIN_CAVE_RUN) continue;
        /* Keep the proven A40 placement when its full 0x200-byte tail exists;
         * otherwise use the start of a uniquely trailing zero run. */
        uint32_t pos = usable - tail >= 0x200u ? usable - 0x200u : start;
        *raw = s->raw + pos;
        *rva = s->rva + pos;
        if (!range_ok(size, *raw, TRAMPOLINE_BYTES) ||
            !encode_b(hook, *rva, &branch) ||
            !encode_b(*rva + 52u, hook + 4u, &branch) ||
            !encode_adr(*rva + 12u, project, 10, &adr)) continue;
        bool zero = true;
        for (uint32_t j = pos; j < usable; ++j) {
            if (!range_ok(size, s->raw + j, 1) || data[s->raw + j] != 0) {
                zero = false;
                break;
            }
        }
        if (zero && !directory_hits(data, size, *rva,
                                    TRAMPOLINE_BYTES) &&
            !relocation_hits(data, size, pe, *rva,
                                     TRAMPOLINE_BYTES) &&
            !branch_targets_cave(data, s, usable, *rva)) return true;
    }
    return false;
}

bool patch_sku_25001_to_31(char* buffer, int32_t size) {
    PeImage pe;
    uint32_t sku_string, project_string, sku_raw, sku_rva;
    uint32_t project_raw, project_rva, project_page, project_imm;
    uint32_t project_buf, project_buf_raw, project_flags;
    uint32_t hook_raw, hook_rva, cave_raw, cave_rva, branch, original_and;
    uint8_t project_reg, output_reg;
    uint32_t words[TRAMPOLINE_WORDS];

    if (!parse_pe(buffer, size, &pe) ||
        !unique_string(buffer, size, &pe, SKU_TEXT, &sku_string) ||
        !unique_string(buffer, size, &pe, PROJECT_TEXT, &project_string) ||
        !unique_x2_ref(buffer, size, &pe, sku_string, &sku_raw, &sku_rva) ||
        !unique_x2_ref(buffer, size, &pe, project_string,
                       &project_raw, &project_rva) ||
        project_rva >= sku_rva || sku_rva - project_rva > 0x1000u ||
        project_raw < 12 || !range_ok(size, project_raw, 16) ||
        !decode_mov_x0(read_instr(buffer, (int32_t)project_raw + 4),
                       &project_reg) ||
        !decode_movz_w1_size(read_instr(buffer, (int32_t)project_raw + 8)) ||
        (read_instr(buffer, (int32_t)project_raw + 12) & 0xfc000000u) != 0x94000000u ||
        !decode_adrp(read_instr(buffer, (int32_t)project_raw - 12),
                     project_rva - 12u, project_reg, &project_page) ||
        !decode_add_x_imm(read_instr(buffer, (int32_t)project_raw - 8),
                          project_reg, &project_imm)) {
        printf("SKU: unique bootconfig/project output signature not found\n");
        return false;
    }
    project_buf = project_page + project_imm;
    if (!rva_to_raw(&pe, project_buf, 27, &project_buf_raw, &project_flags) ||
        !(project_flags & 0x80000000u)) {
        printf("SKU: project-name buffer is not writable PE data\n");
        return false;
    }

    hook_raw = sku_raw + 4u;
    hook_rva = sku_rva + 4u;
    if (!range_ok(size, hook_raw, 16)) {
        printf("SKU: truncated output hook\n");
        return false;
    }
    original_and = read_instr(buffer, (int32_t)hook_raw);
    if (!is_and_w3_low8(original_and) ||
        !decode_mov_x0(read_instr(buffer, (int32_t)hook_raw + 4),
                       &output_reg) ||
        !decode_movz_w1_size(read_instr(buffer, (int32_t)hook_raw + 8)) ||
        (read_instr(buffer, (int32_t)hook_raw + 12) & 0xfc000000u) != 0x94000000u ||
        !choose_cave(buffer, size, &pe, hook_rva, project_buf,
                     &cave_raw, &cave_rva)) {
        printf("SKU: unsafe output hook or executable tail cave\n");
        return false;
    }

    words[0] = original_and;         /* original output argument instruction */
    words[1] = 0x71005c7fu;         /* cmp w3, #23 */
    words[2] = 0x54000161u;         /* b.ne done */
    if (!encode_adr(cave_rva + 12u, project_buf, 10, &words[3]) ||
        !encode_b(cave_rva + 52u, hook_rva + 4u, &words[13]) ||
        !encode_b(hook_rva, cave_rva, &branch)) return false;
    words[4] = 0xb8415149u;         /* ldur w9, [x10,#21] */
    words[5] = 0x5286a64bu;         /* movz w11,#0x3532 */
    words[6] = 0x72a6060bu;         /* movk w11,#0x3030,lsl#16 */
    words[7] = 0x6b0b013fu;         /* cmp w9,w11 */
    words[8] = 0x540000a1u;         /* b.ne done */
    words[9] = 0x78419149u;         /* ldurh w9,[x10,#25] */
    words[10] = 0x7100c53fu;        /* cmp w9,#'1' */
    words[11] = 0x528003ebu;        /* mov w11,#31 */
    words[12] = 0x1a830163u;        /* csel w3,w11,w3,eq */

    for (uint32_t i = 0; i < TRAMPOLINE_WORDS; ++i)
        write_instr(buffer, (int32_t)cave_raw + (int32_t)i * 4, words[i]);
    write_instr(buffer, (int32_t)hook_raw, branch);
    printf("SKU: isolated bootconfig output 25001/23 -> 31, hook RVA 0x%X, cave RVA 0x%X\n",
           hook_rva, cave_rva);
    return true;
}
