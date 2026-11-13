/*
 * Copyright (C) 2026 hqzing
 * SPDX-License-Identifier: 0BSD
 * Repository: https://github.com/hqzing/ohos-selfsign
 *
 * Licensed under the BSD Zero Clause License.
 */

/*
 * selfsign.c — 轻量级 OpenHarmony 二进制自签名工具
 *
 * 用法:
 *     cc selfsign.c -o selfsign
 *     ./selfsign <input_elf> [output_elf] [--force] [--strip] [--check]
 *         缺省 output 时, inplace 改写 input.
 *         --force : 若已含 .codesign 段, 先剥离再重签
 *         --strip : 仅剥离 .codesign 段, 不做签名
 *         --check : 只读校验已有自签名的有效性, 不写任何文件
 *         --refresh: 原地重算已有 .codesign 段的 fileSize/根哈希/签名,
 *                    不改变文件布局 (用于链接后又追加了数据的 ELF,
 *                    例如 DuckDB 在扩展末尾追加的元数据)
 *
 * ─────────────────────────────────────────────────────────────
 * 函数划分说明:
 *
 * 一、签名必需的算法核心 (缺一不可, 组成 sign_elf 主流程):
 *     sha256 / merkle_root_hash / build_descriptor /
 *     inject_codesign_section / sign_elf
 *
 * 二、ELF 预清洗 / 标准化动作 (并非签名所必需, 是签名前的
 *      "把 ELF 恢复到干净、可签状态" 的辅助动作, 已独立成函数,
 *      签名流程只有 --force 时才按需调用):
 *     parse_elf_header     — 校验并解析 ELF64 header (预检, 只读)
 *     find_section_by_name — 按名字在 SHT 里找段 (预检, 只读)
 *     has_codesign_section — 检测是否已含 .codesign 段 (预检, 只读)
 *     strip_codesign       — 剥离已有 .codesign 段并重建 SHT/shstrtab
 *                            (标准化; 纯加签模式下不执行)
 *     check_selfsign       — 只读校验已有自签名的有效性 (--check)
 *
 * 三、文件 I/O 层 (与签名算法无关的健壮性):
 *     sign_file_atomic     — 临时文件 + rename 原子落盘, 保留原权限位
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ─────────────────────────── SHA-256 ─────────────────────────── */
/* 按 FIPS 180-4 (Secure Hash Standard, 公开规范) 自实现 */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} SHA256_CTX;

/* FIPS 180-4 §4.2.2: SHA-256 的 64 个轮常量 K (κ = floor(2^32 * sin(i)) 的前32位) */
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

/* FIPS 180-4 §3.1.1: 32 位字上的轮函数 (ROTRr / SHRr) 及压缩函数 Σ/Ch/Maj */
#define ROTR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA_BIG_S0(x) (ROTR32(x,2)  ^ ROTR32(x,13) ^ ROTR32(x,22))   /* Σ0 */
#define SHA_BIG_S1(x) (ROTR32(x,6)  ^ ROTR32(x,11) ^ ROTR32(x,25))   /* Σ1 */
#define SHA_SMALL_S0(x) (ROTR32(x,7) ^ ROTR32(x,18) ^ ((x) >> 3))    /* σ0 */
#define SHA_SMALL_S1(x) (ROTR32(x,17) ^ ROTR32(x,19) ^ ((x) >> 10))  /* σ1 */
#define SHA_CH(e,f,g)  (((e) & (f)) ^ ((~(e)) & (g)))                /* Ch */
#define SHA_MAJ(a,b,c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))    /* Maj */

/* FIPS 180-4 §5.3.3: SHA-256 的 8 个初始哈希值 H */
static void sha256_init(SHA256_CTX *c) {
    c->bitlen = 0; c->buflen = 0;
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c; c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
}

/* FIPS 180-4 §5.1.1 + §6.2.2: 对一个 512-bit (64B) 块做压缩, 更新 state */
static void sha256_block(SHA256_CTX *c, const uint8_t *p) {
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i*4]) << 24 | ((uint32_t)p[i*4+1]) << 16
             | ((uint32_t)p[i*4+2]) << 8 | ((uint32_t)p[i*4+3]);
    }
    for (; i < 64; i++) {
        w[i] = SHA_SMALL_S1(w[i-2]) + w[i-7] + SHA_SMALL_S0(w[i-15]) + w[i-16];
    }
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
    for (i = 0; i < 64; i++) {
        t1 = h + SHA_BIG_S1(e) + SHA_CH(e, f, g) + SHA256_K[i] + w[i];
        t2 = SHA_BIG_S0(a) + SHA_MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

/* FIPS 180-4 §5.2: 把字节流按 64B 一块喂给 sha256_block, 维护 bitlen */
static void sha256_update(SHA256_CTX *c, const uint8_t *data, size_t len) {
    c->bitlen += (uint64_t)len * 8;
    while (len--) {
        c->buf[c->buflen++] = *data++;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

/* FIPS 180-4 §5.1.1: padding — 末块补 1bit 0x80 + 0.. 直到剩 8 字节, 末 8 字节填 bitlen 大端 */
static void sha256_final(SHA256_CTX *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen;
    size_t i = c->buflen;
    c->buf[i++] = 0x80;
    if (i > 56) { while (i < 64) c->buf[i++] = 0; sha256_block(c, c->buf); i = 0; }
    while (i < 56) c->buf[i++] = 0;
    for (i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_block(c, c->buf);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    SHA256_CTX c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ─────────────────────────── 字节读写工具 ─────────────────────────── */
/* 小端读取/写入: 一律走 memcpy/显式移位, 不做未对齐指针解引用, 保证在
 * 任何对齐要求的平台 (ARM 等) 上都安全且可移植. */
static uint16_t read_u16(const uint8_t *p, size_t len, size_t off) {
    uint16_t v = 0;
    if (off + 2 <= len) { uint8_t b[2]; memcpy(b, p + off, 2); v = (uint16_t)b[0] | ((uint16_t)b[1] << 8); }
    return v;
}
static uint32_t read_u32(const uint8_t *p, size_t len, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= len) { uint8_t b[4]; memcpy(b, p + off, 4);
        v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
    return v;
}
static uint64_t read_u64(const uint8_t *p, size_t len, size_t off) {
    uint64_t v = 0;
    if (off + 8 <= len) { uint8_t b[8]; memcpy(b, p + off, 8);
        for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i); }
    return v;
}
static void write_u16(uint8_t *p, size_t off, uint16_t v) {
    p[off] = v & 0xff; p[off+1] = (v >> 8) & 0xff;
}
static void write_u32(uint8_t *p, size_t off, uint32_t v) {
    p[off]=v&0xff; p[off+1]=(v>>8)&0xff; p[off+2]=(v>>16)&0xff; p[off+3]=(v>>24)&0xff;
}
static void write_u64(uint8_t *p, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) p[off+i] = (uint8_t)(v >> (8 * i));
}

static uint64_t align_up(uint64_t v, uint64_t a) { return ((v + a - 1) / a) * a; }

/* ─────────────────────────── ELF 常量 ─────────────────────────── */
static const size_t DESC_SIZE = 256;
static const size_t PAGE_SIZE = 4096;
static const uint32_t FLAG_SELF_SIGN = 0x10;
static const uint32_t FS_VERITY_DESCRIPTOR_TYPE = 1;

/* ELF64 header 字段偏移 */
static const size_t E_SHOFF = 0x28;
static const size_t E_SHENTSIZE = 0x3a;
static const size_t E_SHNUM = 0x3c;
static const size_t E_SHSTRNDX = 0x3e;

/* 显式声明为恰好 10 字节 (9 字符 + NUL): 若用字符串字面量加 sizeof,
 * 编译器会隐式追加终止 NUL 得到 11, 会导致注入/剥离出的 shstrtab
 * 长度差 1 字节. */
static const char CODESIGN_NAME[10] = ".codesign\0";

/* ─────────────────── ELF 预清洗/标准化 (非签名必需) ─────────────────── */

/*
 * parse_elf_header_check — 静默核心: 校验并解析 ELF64 header (只读预检).
 *
 * 检查项:
 *  - 校验魔数 \x7fELF + ELFCLASS64
 *  - 校验 e_shentsize >= 64 (ELF64 允许段表条目大于 64 字节以携带扩展
 *    字段, 故只拒绝小于 64 的; 实际条目大小经 e_shentsize 传出, 后续
 *    所有 SHT 遍历/越界校验均按它步进)
 *  - 校验 SHT 不越界 (checked_add 语义), e_shstrndx < e_shnum
 * 返回 NULL 表示通过, 输出 e_shoff/e_shentsize/e_shnum/e_shstrndx;
 * 否则返回错误串 (不打印), 供调用方自行决定如何呈现.
 */
static const char *parse_elf_header_check(const uint8_t *elf, size_t elf_len,
                                          uint64_t *e_shoff, uint16_t *e_shentsize,
                                          uint16_t *e_shnum, uint16_t *e_shstrndx) {
    if (elf_len < 64 || memcmp(elf, "\x7f""ELF", 4) != 0 || elf[4] != 2) {
        return "not ELF64";
    }
    uint64_t shoff = read_u64(elf, elf_len, E_SHOFF);
    uint16_t shentsize = read_u16(elf, elf_len, E_SHENTSIZE);
    uint16_t shnum = read_u16(elf, elf_len, E_SHNUM);
    uint16_t shstrndx = read_u16(elf, elf_len, E_SHSTRNDX);
    if (shentsize < 64 || shoff == 0 || shnum == 0 || (uint64_t)shstrndx >= (uint64_t)shnum) {
        return "ELF has no usable section header table";
    }
    /* SHT 越界检查: e_shoff + e_shnum*e_shentsize 必须落在文件内 (对应 checked_add + 比较) */
    if (shoff > elf_len || shnum > (elf_len - shoff) / shentsize) {
        return "section header table out of bounds";
    }
    *e_shoff = shoff;
    *e_shentsize = shentsize;
    *e_shnum = shnum;
    *e_shstrndx = shstrndx;
    return NULL;
}

/*
 * parse_elf_header — 校验并解析 ELF64 header (只读预检).
 *
 * 这不是签名算法的一部分, 而是签名/剥离共用的"先决条件检查":
 * parse_elf_header_check 的打印包装, 失败时向 stderr 输出
 * "error: <错误串>". 返回 0 表示通过; 返回 -1 表示不是可处理的 ELF64.
 */
int parse_elf_header(const uint8_t *elf, size_t elf_len,
                            uint64_t *e_shoff, uint16_t *e_shentsize,
                            uint16_t *e_shnum, uint16_t *e_shstrndx) {
    const char *err = parse_elf_header_check(elf, elf_len, e_shoff, e_shentsize,
                                             e_shnum, e_shstrndx);
    if (err) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/*
 * find_section_by_name — 在 SHT 中按名字找段 (只读预检).
 *
 * 先经 shstrtab entry 解析出字符串表位置并做越界检查, 再遍历所有段条目
 * 比较 sh_name 指向的名字. 段条目按实际 e_shentsize 步进.
 * 返回段条目在文件中的偏移 (即 e_shoff + idx*e_shentsize), 未找到返回 -1.
 */
int64_t find_section_by_name(const uint8_t *elf, size_t elf_len,
                                    uint64_t e_shoff, uint16_t e_shentsize,
                                    uint16_t e_shnum, uint16_t e_shstrndx,
                                    const char *name) {
    size_t name_len = strlen(name) + 1; /* 含 NUL */
    size_t shstr_e = (size_t)e_shoff + (size_t)e_shstrndx * e_shentsize;
    uint64_t shstr_off = read_u64(elf, elf_len, shstr_e + 24);
    uint64_t shstr_sz = read_u64(elf, elf_len, shstr_e + 32);
    /* 用减法形式避免 shstr_off + shstr_sz 回绕 */
    if (shstr_off > elf_len || shstr_sz > elf_len - shstr_off) return -1;
    for (uint16_t i = 0; i < e_shnum; i++) {
        size_t e = (size_t)e_shoff + (size_t)i * e_shentsize;
        uint32_t name_off = read_u32(elf, elf_len, e);
        if ((uint64_t)name_off + name_len <= shstr_sz) {
            if (memcmp(elf + shstr_off + name_off, name, name_len) == 0) {
                return (int64_t)e;
            }
        }
    }
    return -1;
}

/*
 * has_codesign_section — 检测 ELF 是否已含 .codesign 段 (只读预检).
 *
 * 签名前先问一句"是不是已经签过", 由调用方决定:
 *   - 纯加签模式: 已含则直接报"已签名"错误
 *   - --force 模式: 已含则先走 strip_codesign 再签
 */
int has_codesign_section(const uint8_t *elf, size_t elf_len) {
    uint64_t e_shoff; uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (parse_elf_header(elf, elf_len, &e_shoff, &e_shentsize, &e_shnum, &e_shstrndx) < 0) return 0;
    return find_section_by_name(elf, elf_len, e_shoff, e_shentsize, e_shnum,
                                e_shstrndx, CODESIGN_NAME) >= 0 ? 1 : 0;
}

/* strip 时 shstrtab 在新 SHT 中的索引: 若 .codesign 在 shstrtab 之前被删,
 * shstrtab 的索引整体前移一位, 否则不变. */
static uint16_t new_shstrndx(uint16_t old_shstrndx, size_t cs_idx) {
    return cs_idx < old_shstrndx ? (uint16_t)(old_shstrndx - 1) : old_shstrndx;
}

/*
 * strip_codesign — 剥离 .codesign 段, 重建 shstrtab 与 SHT (ELF 标准化).
 *
 * ⚠ 这是"预清洗/标准化"动作, 并非签名所必需: 签名算法本身只要求输入
 * 是干净的 ELF64; 若输入已带 .codesign 段, 反复加签会累积畸形段.
 * 只有 --force 才会调用它.
 *
 * 剥离步骤:
 *   1. 定位 .codesign 段条目 → cs_idx
 *   2. 从 shstrtab 中 drain 掉 ".codesign\0" (cs_name_off .. +10)
 *   3. 新 SHT = 旧 SHT 去掉 cs_idx 条目 (条目不压缩, 按原 e_shentsize 保留)
 *   4. 新表落位: 段数据之后恰好只有旧表时照旧截断原位重建; 否则保留全部
 *      内容, 新 shstrtab 与 8B 对齐的新 SHT 追加到文件末尾
 *   5. 重写 shstrtab 条目偏移/大小 (注意 cs_idx 在 shstrtab 前后时索引调整)
 *   6. 所有 sh_name > cs_name_off 的段名偏移整体前移 cs_name_len
 *   7. 更新 header: e_shoff / e_shnum / e_shstrndx (仅 cs_idx < e_shstrndx 时 -1)
 *
 * 入参 *buf / *buf_len 会被原地改写 (可能 realloc).
 * 返回 1 = 已剥离; 0 = 本来就没有 .codesign; -1 = 错误.
 */
int strip_codesign(uint8_t **buf, size_t *buf_len) {
    uint8_t *elf = *buf;
    size_t elf_len = *buf_len;
    uint64_t e_shoff; uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (parse_elf_header(elf, elf_len, &e_shoff, &e_shentsize, &e_shnum, &e_shstrndx) < 0) return -1;

    int64_t cs_entry_off = find_section_by_name(elf, elf_len, e_shoff, e_shentsize,
                                                e_shnum, e_shstrndx, CODESIGN_NAME);
    if (cs_entry_off < 0) return 0; /* 无 .codesign, 无需清洗 */
    size_t cs_idx = ((size_t)cs_entry_off - (size_t)e_shoff) / e_shentsize;

    /* 读 shstrtab 位置并做越界检查 */
    size_t shstr_e = (size_t)e_shoff + (size_t)e_shstrndx * e_shentsize;
    size_t shstr_off = (size_t)read_u64(elf, elf_len, shstr_e + 24);
    size_t shstr_sz = (size_t)read_u64(elf, elf_len, shstr_e + 32);
    if (shstr_off > elf_len || shstr_sz > elf_len - shstr_off) {
        fprintf(stderr, "error: shstrtab out of bounds\n");
        return -1;
    }

    /* 2. 新 shstrtab = 旧 shstrtab 删掉 ".codesign\0" */
    uint32_t cs_name_off = read_u32(elf, elf_len, (size_t)cs_entry_off);
    size_t cs_name_len = sizeof(CODESIGN_NAME); /* 10, 含 NUL */
    size_t new_shstr_sz = shstr_sz;
    uint8_t *new_shstr = malloc(shstr_sz);
    if (!new_shstr) { fprintf(stderr, "error: out of memory\n"); return -1; }
    memcpy(new_shstr, elf + shstr_off, shstr_sz);
    if (cs_name_off + cs_name_len <= shstr_sz) {
        memmove(new_shstr + cs_name_off, new_shstr + cs_name_off + cs_name_len,
                shstr_sz - cs_name_off - cs_name_len);
        new_shstr_sz -= cs_name_len;
    }

    /* 3. 新 SHT = 旧 SHT 去掉 cs_idx 条目 (每条目按 e_shentsize 字节原样拷贝) */
    uint16_t new_shnum = e_shnum - 1;
    size_t new_sht_bytes = (size_t)new_shnum * e_shentsize;
    uint8_t *new_sht = malloc(new_sht_bytes);
    if (!new_sht) { free(new_shstr); fprintf(stderr, "error: out of memory\n"); return -1; }
    size_t dst = 0;
    for (uint16_t i = 0; i < e_shnum; i++) {
        if (i == cs_idx) continue;
        memcpy(new_sht + dst, elf + (size_t)e_shoff + (size_t)i * e_shentsize, e_shentsize);
        dst += e_shentsize;
    }

    /* 4. 新表落位: 若 .codesign 段数据之后恰好只有旧 shstrtab 与旧 SHT
       (即本工具自己签出的布局), 照旧截断到段偏移并在原位重建, 文件不膨胀;
       否则 (如签名后又被 patchelf 等工具重排过, 段数据之后仍有活跃内容,
       例如被挪到文件尾部新增段的 .interp) 不截断, 新表追加到文件末尾,
       原段数据成为无引用的死字节. */
    size_t cs_sec_off = (size_t)read_u64(elf, elf_len, (size_t)cs_entry_off + 24);
    int compact = cs_sec_off <= elf_len
                  && (size_t)cs_sec_off + PAGE_SIZE == shstr_off
                  && (size_t)e_shoff == (size_t)align_up(shstr_off + shstr_sz, 8)
                  && elf_len == (size_t)e_shoff + (size_t)e_shnum * e_shentsize;
    size_t keep_len = compact ? (size_t)cs_sec_off : elf_len;
    size_t new_shstr_off = keep_len;
    size_t new_sht_off = (size_t)align_up(new_shstr_off + new_shstr_sz, 8);
    size_t new_total = new_sht_off + new_sht_bytes;

    uint8_t *out = calloc(1, new_total ? new_total : 1);
    if (!out) { free(new_shstr); free(new_sht); fprintf(stderr, "error: out of memory\n"); return -1; }
    memcpy(out, elf, keep_len);
    memcpy(out + new_shstr_off, new_shstr, new_shstr_sz);
    memcpy(out + new_sht_off, new_sht, new_sht_bytes);
    free(new_shstr);
    free(new_sht);

    /* 5. 重写 shstrtab 条目: 新 shstr 位置/大小 (cs_idx 在 shstrtab 前后时索引不同) */
    size_t shstr_entry_off_in_new = (size_t)new_shstrndx(e_shstrndx, cs_idx) * e_shentsize;
    write_u64(out, new_sht_off + shstr_entry_off_in_new + 24, new_shstr_off);
    write_u64(out, new_sht_off + shstr_entry_off_in_new + 32, new_shstr_sz);

    /* 6. 所有 sh_name > cs_name_off 的段名偏移整体前移 cs_name_len */
    for (uint16_t i = 0; i < new_shnum; i++) {
        size_t e = new_sht_off + (size_t)i * e_shentsize;
        uint32_t noff = read_u32(out, new_total, e);
        if (noff > cs_name_off) write_u32(out, e, noff - (uint32_t)cs_name_len);
    }

    /* 7. 更新 header */
    write_u64(out, E_SHOFF, new_sht_off);
    write_u16(out, E_SHNUM, new_shnum);
    if (cs_idx < e_shstrndx) write_u16(out, E_SHSTRNDX, e_shstrndx - 1);

    free(*buf);
    *buf = out;
    *buf_len = new_total;
    return 1;
}

/* ─────────────────── 签名必需的算法核心 ─────────────────── */

/*
 * inject_codesign_section — 注入 4KB 占位 .codesign 段 (签名第一步).
 *
 * 这是签名算法的必要组成部分, 流程如下:
 *   1. 计算所有段末尾的最大值 cur_end = max(e_shoff+e_shnum*e_shentsize,
 *      各段 off+sz [SHT_NOBITS 不计], 文件实际长度), 段文件偏移
 *      cs_off = align_up(cur_end, 4096)
 *   2. 新 shstrtab = 旧 + ".codesign\0"; 落位 cs_off+4096
 *   3. 新 SHT 落位新 shstrtab 之后 (8B 对齐), 按原 e_shentsize 复制旧 SHT,
 *      追加 .codesign 条目 (条目前 64B 是标准字段: sh_type=SHT_PROGBITS,
 *      sh_offset=cs_off, sh_size=4096, sh_addralign=4096; 超出 64B 的
 *      扩展部分保持 0)
 *   4. 更新 shstrtab 条目偏移/大小, 更新 header e_shoff/e_shnum (e_shstrndx 不变)
 *
 * 关键差异: 只拷贝 [0, min(elf_len, cs_off)) 的原始内容, cs_off 之后到
 * 新布局之间的区域全部保持 0 — cur_end 已计入文件实际长度, 故段表之后
 * 拖着的额外字节 (如 Bun/.NET SingleFile 附加数据) 同样进入产物.
 *
 * 返回 0 成功, *out_len 为产物长度, *cs_off_out 为段文件偏移; -1 失败.
 */
int inject_codesign_section(const uint8_t *elf, size_t elf_len,
                                   uint8_t **out, size_t *out_len,
                                   uint64_t *cs_off_out) {
    uint64_t e_shoff; uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (parse_elf_header(elf, elf_len, &e_shoff, &e_shentsize, &e_shnum, &e_shstrndx) < 0) return -1;

    /* shstrtab entry (用于取旧 shstr 内容) */
    size_t shstr_e = (size_t)e_shoff + (size_t)e_shstrndx * e_shentsize;
    uint64_t shstr_off = read_u64(elf, elf_len, shstr_e + 24);
    uint64_t shstr_sz = read_u64(elf, elf_len, shstr_e + 32);
    if (shstr_off > elf_len || shstr_sz > elf_len - shstr_off) {
        fprintf(stderr, "error: shstrtab out of bounds\n");
        return -1;
    }

    /* 1. cur_end: SHT 末尾与各段 off+sz 的最大值 (SHT_NOBITS=8 不占文件),
       且不小于文件实际长度 (段表之后可能拖着附加数据) */
    uint64_t cur_end = e_shoff + (uint64_t)e_shnum * e_shentsize;
    for (uint16_t i = 0; i < e_shnum; i++) {
        size_t e = (size_t)e_shoff + (size_t)i * e_shentsize;
        uint32_t sh_type = read_u32(elf, elf_len, e + 4);
        uint64_t off = read_u64(elf, elf_len, e + 24);
        uint64_t sz = sh_type == 8 ? 0 : read_u64(elf, elf_len, e + 32);
        if (off + sz > cur_end) cur_end = off + sz;
    }
    if (elf_len > cur_end) cur_end = elf_len;
    uint64_t cs_off = align_up(cur_end, PAGE_SIZE);

    /* 2. 新 shstrtab = 旧 + ".codesign\0" */
    size_t name_len = sizeof(CODESIGN_NAME);
    size_t new_shstr_sz = (size_t)shstr_sz + name_len;
    uint8_t *new_shstr = malloc(new_shstr_sz);
    if (!new_shstr) { fprintf(stderr, "error: out of memory\n"); return -1; }
    memcpy(new_shstr, elf + shstr_off, (size_t)shstr_sz);
    memcpy(new_shstr + shstr_sz, CODESIGN_NAME, name_len);
    uint32_t cs_shname = (uint32_t)shstr_sz; /* .codesign 在新 shstrtab 内的偏移 */

    /* 3. 新布局: 段在 cs_off, 新 shstrtab 在 cs_off+4096, 新 SHT 8B 对齐之后 */
    uint64_t new_shstr_off = cs_off + PAGE_SIZE;
    uint64_t new_sht_off = align_up(new_shstr_off + new_shstr_sz, 8);
    uint16_t new_shnum = e_shnum + 1;
    size_t new_total = (size_t)new_sht_off + (size_t)new_shnum * e_shentsize;

    uint8_t *buf = calloc(1, new_total);
    if (!buf) { free(new_shstr); fprintf(stderr, "error: out of memory\n"); return -1; }

    /* 4. 拷贝原内容: 只拷到 cs_off */
    size_t copy_len = elf_len < new_total ? elf_len : new_total;
    if (copy_len > cs_off) copy_len = (size_t)cs_off;
    if (copy_len) memcpy(buf, elf, copy_len);
    /* cs_off..cs_off+4096 为段占位 (calloc 全 0), 稍后写入 payload */

    memcpy(buf + new_shstr_off, new_shstr, new_shstr_sz);
    memcpy(buf + new_sht_off, elf + (size_t)e_shoff, (size_t)e_shnum * e_shentsize);
    free(new_shstr);

    /* .codesign 段条目 (前 64B 为标准字段, 超出 64B 的扩展部分保持 0) */
    size_t cs_e = (size_t)new_sht_off + (size_t)e_shnum * e_shentsize;
    write_u32(buf, cs_e + 0, cs_shname);            /* sh_name */
    write_u32(buf, cs_e + 4, 1);                    /* sh_type = SHT_PROGBITS */
    /* sh_flags/sh_addr 保持 0 */
    write_u64(buf, cs_e + 24, cs_off);              /* sh_offset */
    write_u64(buf, cs_e + 32, PAGE_SIZE);           /* sh_size */
    /* sh_link/sh_info 保持 0 */
    write_u64(buf, cs_e + 48, PAGE_SIZE);           /* sh_addralign */
    /* sh_entsize 保持 0 */

    /* 更新 shstrtab 条目偏移/大小 (在新 SHT 中的索引仍为 e_shstrndx) */
    size_t shstr_e_new = (size_t)new_sht_off + (size_t)e_shstrndx * e_shentsize;
    write_u64(buf, shstr_e_new + 24, new_shstr_off);
    write_u64(buf, shstr_e_new + 32, new_shstr_sz);

    /* 更新 header: e_shoff / e_shnum; e_shstrndx 不变 */
    write_u64(buf, E_SHOFF, new_sht_off);
    write_u16(buf, E_SHNUM, new_shnum);

    *out = buf;
    *out_len = new_total;
    *cs_off_out = cs_off;
    return 0;
}

/*
 * merkle_root_hash — fs-verity Merkle 树根哈希 + 中间层哈希 (签名必需).
 *
 * 与上游 merkle_tree_builder.cpp::RunHashTask 等价:
 *   - 叶层: 每 4096B 一页 SHA-256, 末页零填充; 段所在页
 *     [cs_off/PAGE, ceil((cs_off+cs_len)/PAGE)) 的叶哈希全置 0
 *   - 上推: 每页打包 128 个 32B 哈希再 SHA-256, 末页零填充, 直到
 *     整层 packed <= 4096, 补零后哈希即根
 *
 * 根哈希写入 root[32]; 中间层 (叶层与根所在层之间的所有层, 从低往高
 * 逐层把 32B 哈希按顺序拼接, 叶层 <= 4096B 时不存在) 经 *mid_out /
 * *mid_len_out 传出, 由调用方 free, 签名时写入 .codesign 段内
 * payload 之后.
 */
void merkle_root_hash(const uint8_t *data, size_t len,
                             uint64_t cs_off, uint64_t cs_len,
                             uint8_t root[32],
                             uint8_t **mid_out, size_t *mid_len_out) {
    const size_t PAGE = 4096, H = 32;
    uint8_t page[PAGE];
    uint8_t *mid = NULL;
    size_t mid_len = 0;
    *mid_out = NULL;
    *mid_len_out = 0;
    if (len == 0) {
        memset(page, 0, PAGE); sha256(page, PAGE, root); return;
    }
    size_t npages = (len + PAGE - 1) / PAGE;
    uint8_t *cur = malloc(npages * H);
    size_t cs_page_begin = (size_t)(cs_off / PAGE);
    size_t cs_page_end   = (size_t)((cs_off + cs_len + PAGE - 1) / PAGE);
    for (size_t i = 0; i < npages; i++) {
        if (cs_len > 0 && i >= cs_page_begin && i < cs_page_end) {
            memset(cur + i * H, 0, H);
            continue;
        }
        memset(page, 0, PAGE);
        size_t off = i * PAGE;
        size_t n = (off + PAGE <= len) ? PAGE : (len - off);
        memcpy(page, data + off, n);
        sha256(page, PAGE, cur + i * H);
    }
    if (npages == 1) { memcpy(root, cur, H); free(cur); return; }
    size_t ncur = npages;
    while (ncur * H > PAGE) {
        size_t packed = ncur * H;
        size_t next_pages = (packed + PAGE - 1) / PAGE;
        uint8_t *next = malloc(next_pages * H);
        for (size_t i = 0; i < next_pages; i++) {
            memset(page, 0, PAGE);
            size_t off = i * PAGE;
            size_t n = (off + PAGE <= packed) ? PAGE : (packed - off);
            memcpy(page, cur + off, n);
            sha256(page, PAGE, next + i * H);
        }
        if (next_pages * H > PAGE) {
            /* 中间层暂存: 低于根所在层的各层, 从低往高拼接
               (根所在层 packed <= 4096 不属于中间层) */
            uint8_t *grown = realloc(mid, mid_len + next_pages * H);
            if (grown) {
                mid = grown;
                memcpy(mid + mid_len, next, next_pages * H);
                mid_len += next_pages * H;
            }
        }
        free(cur);
        cur = next; ncur = next_pages;
    }
    memset(page, 0, PAGE);
    memcpy(page, cur, ncur * H);
    sha256(page, PAGE, root);
    free(cur);
    *mid_out = mid;
    *mid_len_out = mid_len;
}

/*
 * build_descriptor — 构造 256 字节 fs-verity descriptor (签名必需).
 *
 * 字段布局, 全小端:
 *   off  size  field
 *   0    1     version = 1
 *   1    1     hashAlgorithm = 1 (SHA-256)
 *   2    1     log2BlockSize = 12 (4096)
 *   3    1     saltSize = 0
 *   4    4     signSize (摘要时=0, 落盘时=32)
 *   8    8     fileSize
 *   16   64    rootHash (32B 左对齐, 余 0)
 *   80   32    salt (全 0)
 *   112  4     flags = FLAG_SELF_SIGN 0x10
 *   116  4     reserved1 = 0
 *   120  8     merkleTreeOffset = 0
 *   128  127   reserved2 = 0
 *   255  1     csVersion = 3
 */
void build_descriptor(uint8_t *out /*256*/,
                             uint32_t sign_size, uint64_t file_size,
                             const uint8_t root[32], uint32_t flags) {
    memset(out, 0, 256);
    out[0] = 1;
    out[1] = 1;
    out[2] = 12;
    out[3] = 0;
    write_u32(out, 4, sign_size);
    write_u64(out, 8, file_size);
    memcpy(out + 16, root, 32);
    write_u32(out, 112, flags);
    out[255] = 3;
}

/*
 * sign_elf — 签名主流程 (签名必需): 注入占位段 → merkle → descriptor →
 * signature → 拼 payload 写入段内.
 *
 *   - force == 0: 若已含 .codesign 段 → 报"已签名"错误
 *   - force == 1: 先 strip_codesign 预清洗, 再签
 *
 * 返回 0 成功 (*out 为新产物); 返回 1 表示 already signed; 返回 -1 错误.
 */
int sign_elf(const uint8_t *elf, size_t elf_len, int force,
                    uint8_t **out, size_t *out_len) {
    if (elf_len < 64 || memcmp(elf, "\x7f""ELF", 4) != 0 || elf[4] != 2) {
        fprintf(stderr, "error: not ELF64\n");
        return -1;
    }
    /* 预处理阶段: 已签名时按 force 决定报错或先清洗 */
    uint8_t *buf = malloc(elf_len ? elf_len : 1);
    if (!buf) { fprintf(stderr, "error: out of memory\n"); return -1; }
    memcpy(buf, elf, elf_len);
    size_t buf_len = elf_len;
    int cleaned = 0;
    if (has_codesign_section(buf, buf_len)) {
        if (!force) {
            free(buf);
            fprintf(stderr, "error: already has a .codesign section; "
                            "strip first or use --force\n");
            return 1;
        }
        cleaned = strip_codesign(&buf, &buf_len);
        if (cleaned < 0) { free(buf); return -1; }
    }

    /* 1. 注入 4KB 占位 .codesign 段 */
    uint8_t *tmp = NULL; size_t tmp_len = 0; uint64_t cs_off = 0;
    if (inject_codesign_section(buf, buf_len, &tmp, &tmp_len, &cs_off) < 0) {
        free(buf); return -1;
    }
    free(buf);

    /* 2. merkle 根哈希: 跳过 [cs_off, cs_off+4096); mid 为中间层哈希 */
    uint8_t root[32];
    uint8_t *mid = NULL; size_t mid_len = 0;
    merkle_root_hash(tmp, tmp_len, cs_off, PAGE_SIZE, root, &mid, &mid_len);

    /* 3/4. descriptor(signSize=0) 用于摘要, fileSize = 产物长度 */
    uint8_t desc_for_digest[256];
    build_descriptor(desc_for_digest, 0, (uint64_t)tmp_len, root, FLAG_SELF_SIGN);

    /* 5. signature = SHA256(descriptor) */
    uint8_t signature[32];
    sha256(desc_for_digest, DESC_SIZE, signature);

    /* 6. descriptor(signSize=32) 用于落盘 */
    uint8_t desc_on_disk[256];
    build_descriptor(desc_on_disk, 32, (uint64_t)tmp_len, root, FLAG_SELF_SIGN);

    /* 7. ElfSignInfo: 8B 头 + descriptor 256B + signature 32B = 296B */
    uint8_t payload[8 + DESC_SIZE + 32];
    write_u32(payload, 0, FS_VERITY_DESCRIPTOR_TYPE);   /* type = 1 */
    write_u32(payload, 4, (uint32_t)(DESC_SIZE + 32));  /* length = 288 */
    memcpy(payload + 8, desc_on_disk, DESC_SIZE);
    memcpy(payload + 8 + DESC_SIZE, signature, 32);

    /* 8. 原地写入段内: payload 之后紧跟中间层哈希, 可用容量
       4096-296=3800B, 中间层更长时保留靠叶层一侧的前 3800B */
    if (cs_off + sizeof(payload) > tmp_len) { free(mid); free(tmp); return -1; }
    memcpy(tmp + cs_off, payload, sizeof(payload));
    size_t tail = mid_len;
    if (tail > PAGE_SIZE - sizeof(payload)) tail = PAGE_SIZE - sizeof(payload);
    if (tail) memcpy(tmp + cs_off + sizeof(payload), mid, tail);
    free(mid);

    *out = tmp;
    *out_len = tmp_len;
    (void)cleaned;
    return 0;
}

/* ─────────────────── 只读校验 (--check) ─────────────────── */

/*
 * check_selfsign — 只读校验已有自签名的有效性 (--check 的实现, 只读预检).
 *
 * 校验项 (全部通过才算有效; 多项不符时按此顺序报告第一个):
 *   1. 可解析的 ELF64 (与 parse_elf_header 共享静默核心
 *      parse_elf_header_check, 不向 stderr 打印), 存在 .codesign 段,
 *      且段内 ElfSignInfo 头 type=1 / length=288
 *   2. descriptor 固定字段完好: version=1, hashAlgorithm=1,
 *      log2BlockSize=12, csVersion=3
 *   3. signSize=32 且 fileSize=文件实际大小
 *   4. 存储的 merkle 根哈希 == 对整个文件重算的根
 *   5. 存储的 signature == SHA256(signSize 置 0 后的 descriptor)
 *
 * 明确不校验 merkle 中间层 (payload 296B 之后的区域): 自签名场景内核
 * 只验根哈希, 且 .codesign 段覆盖的页在 merkle 叶层本来就置 0, 篡改
 * 中间层不影响根, 此类篡改下本校验仍通过 (预期行为).
 *
 * 返回 1 = 有效; 返回 0 = 无效且 *reason 指向静态原因串
 * (文案五个实现统一).
 */
int check_selfsign(const uint8_t *elf, size_t elf_len, const char **reason) {
    *reason = NULL;

    /* 1a. ELF64 预检 (与 parse_elf_header 共享静默核心;
     * --check 把所有解析失败统一报 not ELF64) */
    uint64_t e_shoff; uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (parse_elf_header_check(elf, elf_len, &e_shoff, &e_shentsize,
                               &e_shnum, &e_shstrndx) != NULL) {
        *reason = "not ELF64"; return 0;
    }

    /* 1b. 定位 .codesign 段, 段内至少要容得下 296B payload */
    int64_t cs_entry_off = find_section_by_name(elf, elf_len, e_shoff, e_shentsize,
                                                e_shnum, e_shstrndx, CODESIGN_NAME);
    if (cs_entry_off < 0) { *reason = "no .codesign section"; return 0; }
    uint64_t cs_off = read_u64(elf, elf_len, (size_t)cs_entry_off + 24);
    uint64_t cs_size = read_u64(elf, elf_len, (size_t)cs_entry_off + 32);
    const size_t payload_len = 8 + DESC_SIZE + 32;
    if (cs_size < payload_len || cs_off > elf_len || payload_len > elf_len - cs_off) {
        *reason = "bad ElfSignInfo header"; return 0;
    }

    /* 1c. ElfSignInfo 头: type=1, length=288 */
    if (read_u32(elf, elf_len, (size_t)cs_off) != FS_VERITY_DESCRIPTOR_TYPE ||
        read_u32(elf, elf_len, (size_t)cs_off + 4) != (uint32_t)(DESC_SIZE + 32)) {
        *reason = "bad ElfSignInfo header"; return 0;
    }

    const uint8_t *desc = elf + cs_off + 8;
    const uint8_t *sig = elf + cs_off + 8 + DESC_SIZE;

    /* 2. descriptor 固定字段: version/hashAlgorithm/log2BlockSize/csVersion */
    if (desc[0] != 1 || desc[1] != 1 || desc[2] != 12 || desc[255] != 3) {
        *reason = "unsupported descriptor fields"; return 0;
    }

    /* 3. signSize=32 且 fileSize=文件实际大小 */
    if (read_u32(desc, DESC_SIZE, 4) != 32) { *reason = "signSize mismatch"; return 0; }
    if (read_u64(desc, DESC_SIZE, 8) != (uint64_t)elf_len) {
        *reason = "fileSize mismatch"; return 0;
    }

    /* 4. merkle 根哈希: 重算整个文件的根 (中间层忽略, mid 算出即弃) */
    uint8_t root[32];
    uint8_t *mid = NULL; size_t mid_len = 0;
    merkle_root_hash(elf, elf_len, cs_off, PAGE_SIZE, root, &mid, &mid_len);
    free(mid);
    if (memcmp(desc + 16, root, 32) != 0) { *reason = "merkle root mismatch"; return 0; }

    /* 5. signature == SHA256(signSize 置 0 后的 descriptor) */
    uint8_t desc0[DESC_SIZE];
    memcpy(desc0, desc, DESC_SIZE);
    memset(desc0 + 4, 0, 4);
    uint8_t expect[32];
    sha256(desc0, DESC_SIZE, expect);
    if (memcmp(sig, expect, 32) != 0) { *reason = "signature mismatch"; return 0; }

    return 1;
}

/*
 * refresh_selfsign — 重新计算已有 .codesign 段的签名 (不移动任何数据).
 *
 * 用于 ELF 在链接后被追加数据 (例如 DuckDB 在文件末尾追加扩展元数据)
 * 的场景: 文件布局与尾部数据保持原样, descriptor 的 fileSize / 根哈希
 * 覆盖整个文件 (含追加数据), 签名段覆盖的页仍按原规则跳过.
 *
 * 返回 0 成功; -1 失败 (无 .codesign / 解析错误 / 段太小).
 */
int refresh_selfsign(uint8_t *elf, size_t elf_len) {
    uint64_t e_shoff; uint16_t e_shentsize, e_shnum, e_shstrndx;
    if (parse_elf_header_check(elf, elf_len, &e_shoff, &e_shentsize,
                               &e_shnum, &e_shstrndx) != NULL) {
        fprintf(stderr, "error: not ELF64\n");
        return -1;
    }
    int64_t cs_entry_off = find_section_by_name(elf, elf_len, e_shoff,
                                                e_shentsize, e_shnum,
                                                e_shstrndx, CODESIGN_NAME);
    if (cs_entry_off < 0) {
        fprintf(stderr, "error: no .codesign section\n");
        return -1;
    }
    uint64_t cs_off = read_u64(elf, elf_len, (size_t)cs_entry_off + 24);
    uint64_t cs_size = read_u64(elf, elf_len, (size_t)cs_entry_off + 32);
    const size_t payload_len = 8 + DESC_SIZE + 32;
    if (cs_size < PAGE_SIZE || cs_off > elf_len ||
        payload_len > elf_len - cs_off) {
        fprintf(stderr, "error: bad .codesign section\n");
        return -1;
    }

    uint8_t root[32];
    uint8_t *mid = NULL; size_t mid_len = 0;
    merkle_root_hash(elf, elf_len, cs_off, PAGE_SIZE, root, &mid, &mid_len);

    uint8_t desc_for_digest[256];
    build_descriptor(desc_for_digest, 0, (uint64_t)elf_len, root,
                     FLAG_SELF_SIGN);
    uint8_t signature[32];
    sha256(desc_for_digest, DESC_SIZE, signature);

    uint8_t desc_on_disk[256];
    build_descriptor(desc_on_disk, 32, (uint64_t)elf_len, root,
                     FLAG_SELF_SIGN);

    uint8_t payload[8 + DESC_SIZE + 32];
    write_u32(payload, 0, FS_VERITY_DESCRIPTOR_TYPE);
    write_u32(payload, 4, (uint32_t)(DESC_SIZE + 32));
    memcpy(payload + 8, desc_on_disk, DESC_SIZE);
    memcpy(payload + 8 + DESC_SIZE, signature, 32);

    memcpy(elf + cs_off, payload, sizeof(payload));
    size_t tail = mid_len;
    if (tail > PAGE_SIZE - sizeof(payload)) {
        tail = PAGE_SIZE - sizeof(payload);
    }
    if (tail) {
        memcpy(elf + cs_off + sizeof(payload), mid, tail);
    }
    free(mid);
    return 0;
}

/* ─────────────────────────── 文件 I/O 层 ─────────────────────────── */

/*
 * sign_file_atomic — 对文件原子签名 (文件 I/O 层, 与签名算法无关).
 *
 * 签名结果先写入同目录临时文件
 * ("<name>.ohos-signing.<pid>.tmp"), 成功后再 rename 覆盖原文件, 失败
 * 时删除临时文件, 绝不损坏源文件; 同时保留原文件的权限位 (否则临时文件
 * 默认 0644, 会剥掉可执行位).
 *
 * force: 0=已签名则报错; 1=先剥离再重签. 返回 0 成功, 非 0 失败.
 */
int sign_file_atomic(const char *path, int force) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *raw = malloc(sz > 0 ? (size_t)sz : 1);
    if (!raw) { fclose(f); return -1; }
    if (sz > 0 && fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return -1; }
    fclose(f);

    uint8_t *signed_buf = NULL; size_t signed_len = 0;
    int rc = sign_elf(raw, (size_t)sz, force, &signed_buf, &signed_len);
    free(raw);
    if (rc != 0) return rc;

    /* 保留原权限位 */
    struct stat st;
    mode_t mode = 0;
    if (stat(path, &st) == 0) mode = st.st_mode & 07777;

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.ohos-signing.%ld.tmp",
             path, (long)getpid());
    remove(tmp_path);
    f = fopen(tmp_path, "wb");
    if (!f) { free(signed_buf); perror(tmp_path); return -1; }
    if (signed_len && fwrite(signed_buf, 1, signed_len, f) != signed_len) {
        fclose(f); remove(tmp_path); free(signed_buf); return -1;
    }
    fclose(f);
    if (mode) chmod(tmp_path, mode);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path); free(signed_buf); perror("rename"); return -1;
    }
    free(signed_buf);
    return 0;
}

/*
 * refresh_file_atomic — 原地刷新签名 (文件 I/O 层).
 *
 * 与 sign_file_atomic 相同的原子落盘/权限保留策略, 但不改变文件布局:
 * 只重写 .codesign 段内的 descriptor/signature.
 */
int refresh_file_atomic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *raw = malloc(sz > 0 ? (size_t)sz : 1);
    if (!raw) { fclose(f); return -1; }
    if (sz > 0 && fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        free(raw); fclose(f); return -1;
    }
    fclose(f);

    struct stat st;
    int mode = (stat(path, &st) == 0) ? (int)(st.st_mode & 07777) : 0644;

    if (refresh_selfsign(raw, (size_t)sz) != 0) { free(raw); return -1; }

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.ohos-refresh.%ld.tmp", path, (long)getpid());
    remove(tmp_path);
    f = fopen(tmp_path, "wb");
    if (!f) { free(raw); perror(tmp_path); return -1; }
    if (sz > 0 && fwrite(raw, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); remove(tmp_path); free(raw); return -1;
    }
    fclose(f);
    free(raw);
    if (mode) chmod(tmp_path, mode);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path); perror("rename"); return -1;
    }
    return 0;
}

/*
 * 条件编译宏 SELFSIGN_AS_LIBRARY:
 *   定义时跳过 main, 将本文件编译为库:
 *     gcc -DSELFSIGN_AS_LIBRARY -fPIC -shared selfsign.c -o libselfsign.so
 *   未定义时编译为普通命令行可执行程序:
 *     gcc selfsign.c -o selfsign
 */
#ifndef SELFSIGN_AS_LIBRARY
int main(int argc, char **argv) {
    int force = 0, strip_only = 0, check_only = 0, refresh = 0;
    const char *in_path = NULL, *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) force = 1;
        else if (strcmp(argv[i], "--strip") == 0) strip_only = 1;
        else if (strcmp(argv[i], "--check") == 0) check_only = 1;
        else if (strcmp(argv[i], "--refresh") == 0) refresh = 1;
        else if (!in_path) in_path = argv[i];
        else if (!out_path) out_path = argv[i];
        else { fprintf(stderr, "usage: %s <input_elf> [output_elf] [--force] [--strip] [--check] [--refresh]\n", argv[0]); return 1; }
    }
    if (!in_path) {
        fprintf(stderr, "usage: %s <input_elf> [output_elf] [--force] [--strip] [--check] [--refresh]\n"
                "  (output defaults to input, in-place)\n", argv[0]);
        return 1;
    }

    if (check_only) {
        /* --check: 只读校验, 与输出路径/--force/--strip 组合属用法错误 */
        if (force || strip_only || out_path || refresh) {
            fprintf(stderr, "usage: %s <input_elf> [output_elf] [--force] [--strip] [--check] [--refresh]\n"
                    "  (output defaults to input, in-place)\n", argv[0]);
            return 1;
        }
        FILE *f = fopen(in_path, "rb");
        if (!f) { perror(in_path); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); return 2; }
        uint8_t *raw = malloc(sz > 0 ? (size_t)sz : 1);
        if (!raw) { fclose(f); return 2; }
        if (sz > 0 && fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return 2; }
        fclose(f);
        const char *reason = NULL;
        int ok = check_selfsign(raw, (size_t)sz, &reason);
        free(raw);
        if (ok) { printf("check ok: %s\n", in_path); return 0; }
        fprintf(stderr, "check failed: %s (%s)\n", in_path, reason);
        return 1;
    }

    if (refresh) {
        if (force || strip_only || out_path) {
            fprintf(stderr, "usage: %s <input_elf> --refresh\n", argv[0]);
            return 1;
        }
        if (refresh_file_atomic(in_path) != 0) return 2;
        printf("selfsign refresh ok: %s\n", in_path);
        return 0;
    }

    if (!out_path) out_path = in_path;

    if (strip_only) {
        /* --strip: 仅预清洗/标准化, 不做签名 */
        uint8_t *raw = NULL; size_t raw_len = 0;
        FILE *f = fopen(in_path, "rb");
        if (!f) { perror(in_path); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz < 0) { fclose(f); return 2; }
        raw = malloc(sz > 0 ? (size_t)sz : 1);
        if (!raw) { fclose(f); return 2; }
        if (sz > 0 && fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return 2; }
        fclose(f);
        raw_len = (size_t)sz;
        int rc = strip_codesign(&raw, &raw_len);
        if (rc < 0) { free(raw); return 2; }
        if (rc == 0) { printf("no .codesign section to strip: %s\n", in_path); free(raw); return 0; }
        FILE *o = fopen(out_path, "wb");
        if (!o) { free(raw); perror(out_path); return 2; }
        if (raw_len && fwrite(raw, 1, raw_len, o) != raw_len) { fclose(o); free(raw); return 2; }
        fclose(o);
        free(raw);
        printf("strip ok: %s → %s (%zu bytes)\n", in_path, out_path, raw_len);
        return 0;
    }

    /* 默认/--force 签名: inplace 时走原子落盘, 否则直接写出 */
    if (strcmp(in_path, out_path) == 0) {
        int rc = sign_file_atomic(in_path, force);
        if (rc == 1) return 3; /* already signed */
        if (rc != 0) return 2;
        printf("selfsign ok: %s (in-place, %s)\n", in_path, force ? "force" : "append-only");
        return 0;
    }
    uint8_t *raw = NULL;
    FILE *f = fopen(in_path, "rb");
    if (!f) { perror(in_path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 2; }
    raw = malloc(sz > 0 ? (size_t)sz : 1);
    if (!raw) { fclose(f); return 2; }
    if (sz > 0 && fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return 2; }
    fclose(f);
    uint8_t *signed_buf = NULL; size_t signed_len = 0;
    int rc = sign_elf(raw, (size_t)sz, force, &signed_buf, &signed_len);
    free(raw);
    if (rc == 1) return 3;
    if (rc != 0) return 2;
    FILE *o = fopen(out_path, "wb");
    if (!o) { free(signed_buf); perror(out_path); return 2; }
    if (signed_len && fwrite(signed_buf, 1, signed_len, o) != signed_len) { fclose(o); free(signed_buf); return 2; }
    fclose(o);
    free(signed_buf);
    printf("selfsign ok: %s → %s (%zu bytes)\n", in_path, out_path, signed_len);
    return 0;
}
#endif
