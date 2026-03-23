/*
    先patch ABL GBL（如果存在），再找 Boot State 锚点并patch，最后反向追踪 LDRB->STRB 链并就地patch源头LDRB
    然后使用数据流追踪确保ancher之后的 一个STRB 被正确patch成 sink（写WZR）
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

// ==================== 文件读取 ====================

int read_file(const char* filename, unsigned char** data, size_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) return -1;
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    *data = new unsigned char[*size];
    if (fread(*data, 1, *size, file) != *size) {
        delete[] *data;
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

// ==================== GBL patch ====================

int patch_abl_gbl(char* buffer, size_t size) {
    char target[]= "e\0f\0i\0s\0p";
    char replacement[] = "n\0u\0l\0l\0s";
    for (size_t i = 0; i < size - sizeof(target); ++i) {
        if (memcmp(buffer + i, target, sizeof(target)) == 0) {
            memcpy(buffer + i, replacement, sizeof(replacement));
            return 0;
        }
    }
    return -1;
}

// ==================== Boot State锚点 ====================

int16_t Original[] = {
    -1,0x00,0x00,0x34,0x28,0x00,0x80,0x52,
    0x06,0x00,0x00,0x14,0xE8,-1,0x40,0xF9,
    0x08,0x01,0x40,0x39,0x1F,0x01,0x00,0x71,
    0xE8,0x07,0x9F,0x1A,0x08,0x79,0x1F,0x53
};
int16_t Patched[] = {
    -1,-1,-1,-1,0x08,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1
};

int patch_abl_bootstate(char* buffer, size_t size,int8_t* lock_register_num, int* offset) {
    size_t pattern_len = sizeof(Original) / sizeof(int16_t);
    int patched_count = 0;
    if (size < pattern_len) return 0;
    for (size_t i = 0; i <= size - pattern_len; ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern_len; ++j) {
            if (Original[j] != -1 &&
                (unsigned char)buffer[i+j] != (unsigned char)Original[j]) {
                match = false; break;
            }
        }
        if (match) {
            *lock_register_num = *(int8_t*)(&buffer[i]) & 0x1F;
            *offset = (int)i;
            for (size_t j = 0; j < pattern_len; ++j)
                if (Patched[j] != -1) buffer[i+j] = (char)Patched[j];
            patched_count++;
            i += pattern_len - 1;
        }
    }
    return patched_count;
}

// ==================== 基础工具 ====================

static uint32_t read_instr(const char* buf, int off) {
    return (uint8_t)buf[off]|
           ((uint8_t)buf[off+1] << 8)  |
           ((uint8_t)buf[off+2] << 16) |
           ((uint8_t)buf[off+3] << 24);
}
static void write_instr(char* buf, int off, uint32_t val) {
    buf[off]   = (char)(val & 0xFF);
    buf[off+1] = (char)((val >> 8)  & 0xFF);
    buf[off+2] = (char)((val >> 16) & 0xFF);
    buf[off+3] = (char)((val >> 24) & 0xFF);
}

#define function_start 0xd503233fu
static bool is_function_start(const char* buf, int off) {
    return read_instr(buf, off) == function_start;
}

// ==================== 数据位置追踪集合 ====================

struct DataLoc {
    enum Type { REG, STK64, STK8} type;
    int32_t val;
    bool operator==(const DataLoc& o) const {
        return type == o.type && val == o.val;
    }
};

struct LocSet {
    std::vector<DataLoc> locs;

    bool has(DataLoc l) const {
        return std::find(locs.begin(), locs.end(), l) != locs.end();
    }
    bool has_reg(int8_t  r)const { return has({DataLoc::REG,r});}
    bool has_stk64(uint32_t i)  const { return has({DataLoc::STK64, (int32_t)i}); }
    bool has_stk8 (uint32_t i)  const { return has({DataLoc::STK8,  (int32_t)i}); }

    void add(DataLoc l){ if (!has(l)) locs.push_back(l); }
    void add_reg  (int8_t  r)  { add({DataLoc::REG,   r});          }
    void add_stk64(uint32_t i) { add({DataLoc::STK64, (int32_t)i}); }
    void add_stk8 (uint32_t i) { add({DataLoc::STK8,  (int32_t)i}); }

    void del(DataLoc l) {
        locs.erase(std::remove(locs.begin(), locs.end(), l), locs.end());
    }
    void del_reg  (int8_t  r)  { del({DataLoc::REG,   r});          }
    void del_stk64(uint32_t i) { del({DataLoc::STK64, (int32_t)i}); }
    void del_stk8 (uint32_t i) { del({DataLoc::STK8,  (int32_t)i}); }

    bool empty() const { return locs.empty(); }

    void print() const {
        printf("  LocSet{");
        for (size_t i = 0; i < locs.size(); i++) {
            if (i) printf(", ");
            switch (locs[i].type) {
                case DataLoc::REG:   printf("W%d",locs[i].val); break;
                case DataLoc::STK64: printf("[SP+0x%X]/64", locs[i].val); break;
                case DataLoc::STK8:  printf("[SP+0x%X]/8",  locs[i].val); break;
            }
        }
        printf("}\n");
    }
};

// ==================== 辅助函数 ====================

bool is_ldrb(const char* buffer, size_t offset) {
    return (read_instr(buffer,(int)offset) & 0xFFC00000) == 0x39400000;
}
int8_t dump_register_from_LDRB(const char* instr) {
    return (int8_t)((uint8_t)instr[0] & 0x1F);
}
bool is_strb(const char* buffer, size_t offset) {
    uint32_t instr = read_instr(buffer,(int)offset);
    if ((instr & 0xFFC00000) == 0x39000000) return true;
    if ((instr & 0xFFE00C00) == 0x38000000) return true;
    if ((instr & 0xFFE00C00) == 0x38000C00) return true;
    return false;
}
bool is_ldr_x_sp(const char* buffer, size_t offset,int8_t target_reg, uint32_t* imm_out) {
    uint32_t instr = read_instr(buffer,(int)offset);
    if ((instr & 0xFFC00000) != 0xF9400000) return false;
    if ((instr & 0x1F) != (uint8_t)target_reg) return false;
    if (((instr >> 5) & 0x1F) != 31) return false;
    *imm_out = ((instr >> 10) & 0xFFF) << 3;
    return true;
}
bool is_str_x_sp(const char* buffer, size_t offset,
                  uint32_t expected_imm, int8_t* src_reg_out) {
    uint32_t instr = read_instr(buffer,(int)offset);
    if ((instr & 0xFFC00000) != 0xF9000000) return false;
    if (((instr >> 5) & 0x1F) != 31) return false;
    if ((((instr >> 10) & 0xFFF) << 3) != expected_imm) return false;
    *src_reg_out = (int8_t)(instr & 0x1F);
    return true;
}
bool is_ldrb_sp(const char* buffer, size_t offset,
                 int8_t target_reg, uint32_t* imm_out) {
    uint32_t instr = read_instr(buffer,(int)offset);
    if ((instr & 0xFFC00000) != 0x39400000) return false;
    if ((instr & 0x1F) != (uint8_t)target_reg) return false;
    if (((instr >> 5) & 0x1F) != 31) return false;
    *imm_out = (instr >> 10) & 0xFFF;
    return true;
}
bool is_strb_sp(const char* buffer, size_t offset,
                 uint32_t expected_imm, int8_t* src_reg_out) {
    uint32_t instr = read_instr(buffer,(int)offset);
    if ((instr & 0xFFC00000) != 0x39000000) return false;
    if (((instr >> 5) & 0x1F) != 31) return false;
    if (((instr >> 10) & 0xFFF) != expected_imm) return false;
    *src_reg_out = (int8_t)(instr & 0x1F);
    return true;
}

// ==================== 向下数据流追踪 ====================
static int track_forward_patch_strb(char* buffer, size_t size,int   ldrb_off,
                                      int8_t src_reg,
                                      int anchor_off)
{
    LocSet set;
    set.add_reg(src_reg);

    printf("\n=== Forward tracking from LDRB@0x%X (W%d), anchor=0x%X ===\n",
           ldrb_off, (int)src_reg, anchor_off);
    set.print();

    for (int off = ldrb_off + 4; off < (int)size - 4; off += 4) {

        if (is_function_start(buffer, off)) {
            printf("0x%X: function boundary, stop\n", off);
            break;
        }
        if (set.empty()) {
            printf("  LocSet empty, stop\n");
            break;
        }

        uint32_t instr = read_instr(buffer, off);
        uint8_t  rt= instr & 0x1F;
        uint8_t  rn    = (instr >> 5) & 0x1F;

        // ---------- STRXt, [SP, #imm]64-bit spill ----------
        if ((instr & 0xFFC00000) == 0xF9000000 && rn == 31) {
            uint32_t imm = ((instr >> 10) & 0xFFF) << 3;
            if (set.has_reg((int8_t)rt)) {
                printf("  0x%X: STR X%d,[SP,#0x%X] spill64\n", off, rt, imm);
                set.add_stk64(imm); set.print();
            } else if (set.has_stk64(imm)) {
                printf("  0x%X: STR X%d,[SP,#0x%X] overwrite stk64 -> del\n", off, rt, imm);
                set.del_stk64(imm);
            }continue;
        }

        // ---------- LDR Xt, [SP, #imm]  64-bit reload ----------
        if ((instr & 0xFFC00000) == 0xF9400000 && rn == 31) {
            uint32_t imm = ((instr >> 10) & 0xFFF) << 3;
            if (set.has_stk64(imm)) {
                printf("  0x%X: LDR X%d,[SP,#0x%X] reload64\n", off, rt, imm);
                set.add_reg((int8_t)rt); set.print();
            } else if (set.has_reg((int8_t)rt)) {
                printf("  0x%X: LDR X%d,[SP,#0x%X] overwrite reg -> del\n", off, rt, imm);
                set.del_reg((int8_t)rt);
            }
            continue;
        }

        // ---------- STR Wt, [SP, #imm]  32-bit spill ----------
        if ((instr & 0xFFC00000) == 0xB9000000 && rn == 31) {
            uint32_t imm = ((instr >> 10) & 0xFFF) << 2;
            if (set.has_reg((int8_t)rt)) {
                printf("  0x%X: STR W%d,[SP,#0x%X] spill32\n", off, rt, imm);
                set.add_stk64(imm); set.print();
            } else if (set.has_stk64(imm)) {
                printf("  0x%X: STR W%d,[SP,#0x%X] overwrite stk -> del\n", off, rt, imm);
                set.del_stk64(imm);
            }
            continue;
        }

        // ---------- LDR Wt, [SP, #imm]  32-bit reload ----------
        if ((instr & 0xFFC00000) == 0xB9400000 && rn == 31) {
            uint32_t imm = ((instr >> 10) & 0xFFF) << 2;
            if (set.has_stk64(imm)) {
                printf("  0x%X: LDR W%d,[SP,#0x%X] reload32\n", off, rt, imm);
                set.add_reg((int8_t)rt); set.print();
            } else if (set.has_reg((int8_t)rt)) {
                printf("  0x%X: LDR W%d,[SP,#0x%X] overwrite reg -> del\n", off, rt, imm);
                set.del_reg((int8_t)rt);
            }
            continue;
        }

        // ---------- LDRB Wt, [任意Xn, #imm] — 外部内存覆写寄存器 ----------
        // 注意：仅当这是普通 LDRB（非 sink），才处理覆写
        // STRB 在下面单独判断，所以这里只处理 LDRB
        if ((instr & 0xFFC00000) == 0x39400000) {
            // 不管Rn 是不是 SP，LDRB 都是读操作，写入Wt
            if (set.has_reg((int8_t)rt)) {
                printf("  0x%X: LDRB W%d,[X%d,#0x%X] overwrite reg -> del\n",
                       off, rt, rn, (instr >> 10) & 0xFFF);
                set.del_reg((int8_t)rt);
            }
            continue;
        }

        // ---------- MOV Xd, Xm ----------
        if ((instr & 0xFFE0FFE0) == 0xAA0003E0) {
            uint8_t rd = instr & 0x1F;
            uint8_t rm = (instr >> 16) & 0x1F;
            if (set.has_reg((int8_t)rm) && rd != 31) {
                printf("  0x%X: MOV X%d,X%d propagate\n", off, rd, rm);
                set.add_reg((int8_t)rd); set.print();
            } else if (set.has_reg((int8_t)rd)) {
                printf("  0x%X: MOV X%d,X%d overwrite -> del\n", off, rd, rm);
                set.del_reg((int8_t)rd);
            }
            continue;
        }

        // ---------- MOVWd, Wm ----------
        if ((instr & 0xFFE0FFE0) == 0x2A0003E0) {
            uint8_t rd = instr & 0x1F;
            uint8_t rm = (instr >> 16) & 0x1F;
            if (set.has_reg((int8_t)rm) && rd != 31) {
                printf("  0x%X: MOV W%d,W%d propagate\n", off, rd, rm);
                set.add_reg((int8_t)rd); set.print();
            } else if (set.has_reg((int8_t)rd)) {
                printf("  0x%X: MOV W%d,W%d overwrite -> del\n", off, rd, rm);
                set.del_reg((int8_t)rd);
            }
            continue;
        }

        // ==========================================================
        // STRB — 统一判断，不区分 SP / 非SP
        // 三种编码形式：unsigned-offset / post-index / pre-index
        // ==========================================================
        {
            bool is_strb_instr = false;
            uint8_t  s_rt = 0, s_rn = 0;
            uint32_t s_imm = 0;

            // unsigned offset: 0x39000000
            if ((instr & 0xFFC00000) == 0x39000000) {
                is_strb_instr = true;
                s_rt= instr & 0x1F;
                s_rn  = (instr >> 5) & 0x1F;
                s_imm = (instr >> 10) & 0xFFF;
            }
            // post-index / pre-index: 0x38000000 / 0x38000C00
            else if ((instr & 0xFFE00C00) == 0x38000000 ||
                     (instr & 0xFFE00C00) == 0x38000C00) {
                is_strb_instr = true;
                s_rt  = instr & 0x1F;
                s_rn  = (instr >> 5) & 0x1F;
                s_imm = (instr >> 12) & 0x1FF; // simm9
            }

            if (is_strb_instr && set.has_reg((int8_t)s_rt)) {
                if (off > anchor_off) {
                    //***找到 sink，patch Rt -> WZR ***
                    const char* rn_str = (s_rn == 31) ? "SP" : "Xn";
                    printf("  0x%X: STRB W%d,[%s,#0x%X] ** SINK (after anchor0x%X) **\n",
                           off, s_rt, rn_str, s_imm, anchor_off);
                    printf("  Before: %02X %02X %02X %02X\n",
                           (uint8_t)buffer[off],(uint8_t)buffer[off+1],
                           (uint8_t)buffer[off+2], (uint8_t)buffer[off+3]);

                    uint32_t patched_instr = (instr & ~0x1Fu) | 31u;
                    write_instr(buffer, off, patched_instr);

                    printf("  After : %02X %02X %02X %02X (Rt -> WZR)\n",
                           (uint8_t)buffer[off],   (uint8_t)buffer[off+1],
                           (uint8_t)buffer[off+2], (uint8_t)buffer[off+3]);
                    return 1; // 成功
                } else {
                    // 锚点之前的 STRB：视为正常 spill，加入集合
                    printf("  0x%X: STRB W%d,[X%d,#0x%X] before anchor -> spill8\n",
                           off, s_rt, s_rn, s_imm);
                    if (s_rn == 31) {
                        // SP-based: 加入 stk8 供后续 LDRB reload追踪
                        set.add_stk8(s_imm);
                    }
                    // 非SP的 STRB 在锚点前：不加入集合（写到外部结构体，不影响追踪）
                    set.print();
                }continue;
            }
        }}

    printf("Forward tracking: no sink STRB found after anchor 0x%X\n", anchor_off);
    return -1;
}

// ==================== 反向找LDRB 源头 + 就地向下追踪 ====================

int find_ldrB_instructio_reverse(char* buffer, size_t size,
                                   int anchor_offset, int8_t target_register) {
    int now_offset= anchor_offset - 4;
    int8_t current_target = target_register;
    int bounce_count  = 0;
    const int MAX_BOUNCES = 8;

    while (now_offset >= 0) {
        if (is_function_start(buffer, now_offset)) {
            printf("Reached function start at 0x%X\n", now_offset);
            break;
        }

        //---- 64-bit 栈 reload弹跳 ----
        uint32_t spill_imm = 0;
        if (is_ldr_x_sp(buffer, now_offset, current_target, &spill_imm)) {
            printf("Bounce at 0x%X: LDR X%d,[SP,#0x%X]\n",
                   now_offset, (int)current_target, spill_imm);
            int search = now_offset - 4;
            bool found = false;
            while (search >= 0) {
                if (is_function_start(buffer, search)) break;
                int8_t src = -1;
                if (is_str_x_sp(buffer, search, spill_imm, &src)) {
                    printf("  -> STR X%d,[SP,#0x%X] at 0x%X\n",
                           (int)src, spill_imm, search);
                    current_target = src;
                    now_offset     = search - 4;
                    found = true; bounce_count++;
                    break;
                }
                search -= 4;
            }
            if (!found) { printf("  -> No matching STR, abort\n"); return -1; }
            if (bounce_count > MAX_BOUNCES) { printf("Too many bounces\n"); return -1; }continue;
        }

        // ---- byte 级栈 reload 弹跳 ----
        uint32_t byte_imm = 0;
        if (is_ldrb_sp(buffer, now_offset, current_target, &byte_imm)) {
            printf("Byte bounce at 0x%X: LDRB W%d,[SP,#0x%X]\n",
                   now_offset, (int)current_target, byte_imm);
            int search = now_offset - 4;
            bool found = false;
            while (search >= 0) {
                if (is_function_start(buffer, search)) break;
                int8_t src = -1;
                if (is_strb_sp(buffer, search, byte_imm, &src)) {
                    printf("  -> STRB W%d,[SP,#0x%X] at 0x%X\n",
                           (int)src, byte_imm, search);
                    current_target = src;
                    now_offset     = search - 4;
                    found = true; bounce_count++;
                    break;
                }
                search -= 4;
            }
            if (!found) { printf("  -> No matching STRB, abort\n"); return -1; }
            if (bounce_count > MAX_BOUNCES) { printf("Too many bounces\n"); return -1; }
            continue;
        }

        // ---- 真正源头: LDRB W{current_target}, [Xn!=SP, #imm] ----
        if (is_ldrb(buffer, now_offset)) {
            uint32_t instr = read_instr(buffer, now_offset);
            uint8_t  rt    = instr & 0x1F;
            uint8_t  rn    = (instr >> 5) & 0x1F;

            if ((int8_t)rt == current_target && rn != 31) {
                printf("Found source LDRB at 0x%X: LDRB W%d,[X%d,#0x%X](%d bounces)\n",
                       now_offset, rt, rn, (instr >> 10) & 0xFFF, bounce_count);
                printf("  Before: %02X %02X %02X %02X\n",
                       (uint8_t)buffer[now_offset],   (uint8_t)buffer[now_offset+1],
                       (uint8_t)buffer[now_offset+2], (uint8_t)buffer[now_offset+3]);

                // Patch 源头 -> MOV Wt, #1
                uint32_t mov_inst = 0x52800020u | (uint8_t)current_target;
                write_instr(buffer, now_offset, mov_inst);

                printf("  After : %02X %02X %02X %02X (MOV W%d, #1)\n",
                       (uint8_t)buffer[now_offset],   (uint8_t)buffer[now_offset+1],
                       (uint8_t)buffer[now_offset+2], (uint8_t)buffer[now_offset+3],
                       (int)current_target);

                // 就地向下追踪 sink STRB
                int fwd = track_forward_patch_strb(
                    buffer, size,
                    now_offset,// 从源头 LDRB 处开始
                    current_target, // 初始寄存器
                    anchor_offset   // 锚点: 只patch 此后的 STRB
                );
                if (fwd <= 0) {
                    printf("Warning: sink STRB not found after anchor 0x%X\n", anchor_offset);
                    return -1;
                }
                printf("Sink patched successfully.\n");
                return 0;
            }
        }

        now_offset -= 4;
    }

    return -1;
}
// ==================== ADRL 地址计算 ====================

// 检查 buffer 中 file_off 处是否以 needle 开头
static bool str_at(const char* buffer, size_t size, int64_t file_off, const char* needle) {
    if (file_off < 0) return false;
    size_t len = strlen(needle);
    if ((size_t)file_off + len >= size) return false;
    return memcmp(buffer + file_off, needle, len) == 0;
}
// ==================== ADRL 三连识别工具 ====================

// 判断是否是 ADRP Xd, #imm
// 编码: [31]=1, [28:24]=10000, bits[23:5]=immhi, bits[4:0]=Rd
static bool is_adrp(uint32_t instr, uint8_t* rd_out) {
    if ((instr & 0x9F000000) != 0x90000000) return false;
    *rd_out = instr & 0x1F;
    return true;
}

// 判断是否是 ADD Xd, Xn, #imm (shifted immediate, 64-bit)
// 编码: 1_00100010_0_imm12_Rn_Rd => 0x91000000
// mask: FF800000, val: 91000000 (shift=0)
//       FF800000, val: 91400000 (shift=12)
static bool is_add_imm_x(uint32_t instr, uint8_t* rd_out, uint8_t* rn_out, uint32_t* imm_out) {
    // sf=1, op=0, S=0, 100010 0/1 imm12 Rn Rd
    if ((instr & 0xFF800000) == 0x91000000) {        // shift=0
        *rd_out  = instr & 0x1F;
        *rn_out  = (instr >> 5) & 0x1F;
        *imm_out = (instr >> 10) & 0xFFF;
        return true;
    }
    if ((instr & 0xFF800000) == 0x91400000) {        // shift=12
        *rd_out  = instr & 0x1F;
        *rn_out  = (instr >> 5) & 0x1F;
        *imm_out = ((instr >> 10) & 0xFFF) << 12;
        return true;
    }
    return false;
}

// 把一条 ADRP 指令的目标寄存器替换为 new_rd
static uint32_t adrp_with_rd(uint32_t instr, uint8_t new_rd) {
    return (instr & ~0x1Fu) | (new_rd & 0x1Fu);
}

// 把一条 ADD Xd, Xn, #imm 的 Rd 和 Rn 都替换为 new_reg
static uint32_t add_with_reg(uint32_t instr, uint8_t new_reg) {
    // 替换 Rd[4:0] 和 Rn[9:5]
    instr = (instr & ~0x1Fu) | (new_reg & 0x1Fu);          // Rd
    instr = (instr & ~(0x1Fu << 5)) | ((uint32_t)(new_reg & 0x1Fu) << 5); // Rn
    return instr;
}

// 从 ADRP+ADD 对计算目标虚拟地址
// adrp_off: ADRP 指令在 buffer 中的文件偏移
// 返回目标的文件偏移（假设文件从 VA 0 加载，或者传入 base 修正）
static int64_t calc_adrl_file_offset(const char* buffer, int adrp_off,
                                      uint64_t load_base = 0) {
    uint32_t i0 = read_instr(buffer, adrp_off);
    uint32_t i1 = read_instr(buffer, adrp_off + 4);

    uint8_t rd0 = 0, rd1 = 0, rn1 = 0;
    uint32_t add_imm = 0;
    if (!is_adrp(i0, &rd0)) return -1;
    if (!is_add_imm_x(i1, &rd1, &rn1, &add_imm)) return -1;
    if (rd1 != rd0 || rn1 != rd0) return -1;

    // ADRP: PC对齐到4K页 + immhi:immlo<<12
    uint64_t pc = load_base + (uint64_t)adrp_off;
    uint64_t page_pc = pc & ~0xFFFull;

    // 提取 ADRP imm: bits[23:5]=immhi(19位), bits[30:29]=immlo(2位)
    uint64_t immlo = (i0 >> 29) & 0x3;
    uint64_t immhi = (i0 >>  5) & 0x7FFFF;
    int64_t  imm   = (int64_t)((immhi << 2) | immlo);
    // 符号扩展到 64 位 (21 位有效)
    if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);
    imm <<= 12;

    uint64_t target_va = (uint64_t)((int64_t)page_pc + imm) + add_imm;

    // 转回文件偏移
    int64_t file_off = (int64_t)(target_va - load_base);
    return file_off;
}

int patch_adrl_unlocked_to_locked(char* buffer, size_t size, uint64_t load_base = 0) {
    if (size < 24) return 0;

    int patched = 0;

    for (size_t i = 0; i <= size - 24; i += 4) {
        uint32_t i0 = read_instr(buffer, (int)(i +  0));
        uint32_t i1 = read_instr(buffer, (int)(i +  4));
        uint32_t i2 = read_instr(buffer, (int)(i +  8));
        uint32_t i3 = read_instr(buffer, (int)(i + 12));
        uint32_t i4 = read_instr(buffer, (int)(i + 16));
        uint32_t i5 = read_instr(buffer, (int)(i + 20));

        // 对 0: ADRP Xa + ADD Xa, Xa
        uint8_t xa = 0, rd1 = 0, rn1 = 0; uint32_t imm1 = 0;
        if (!is_adrp(i0, &xa))                     continue;
        if (!is_add_imm_x(i1, &rd1, &rn1, &imm1))  continue;
        if (rd1 != xa || rn1 != xa)                continue;

        // 对 1: ADRP Xb + ADD Xb, Xb
        uint8_t xb = 0, rd3 = 0, rn3 = 0; uint32_t imm3 = 0;
        if (!is_adrp(i2, &xb))                     continue;
        if (!is_add_imm_x(i3, &rd3, &rn3, &imm3))  continue;
        if (rd3 != xb || rn3 != xb)                continue;

        // 对 2: ADRP Xc + ADD Xc, Xc
        uint8_t xc = 0, rd5 = 0, rn5 = 0; uint32_t imm5 = 0;
        if (!is_adrp(i4, &xc))                     continue;
        if (!is_add_imm_x(i5, &rd5, &rn5, &imm5))  continue;
        if (rd5 != xc || rn5 != xc)                continue;

        // 寄存器两两不同
        if (xa == xb || xb == xc || xa == xc)      continue;

        // ---- 字符串校验（关键！） ----
        int64_t off0 = calc_adrl_file_offset(buffer, (int)(i +  0), load_base);
        int64_t off1 = calc_adrl_file_offset(buffer, (int)(i +  8), load_base);
        int64_t off2 = calc_adrl_file_offset(buffer, (int)(i + 16), load_base);

        if (!str_at(buffer, size, off0, "unlocked"))                            continue;
        if (!str_at(buffer, size, off1, "locked"))                              continue;
        if (!str_at(buffer, size, off2, "androidboot.vbmeta.device_state"))     continue;

        printf("Found ADRL triple at 0x%zX:\n", i);
        printf("  [0x%zX] ADRP+ADD X%d -> file:0x%llX \"unlocked\"\n",
               i,      xa, (unsigned long long)off0);
        printf("  [0x%zX] ADRP+ADD X%d -> file:0x%llX \"locked\"\n",
               i + 8,  xb, (unsigned long long)off1);
        printf("  [0x%zX] ADRP+ADD X%d  -> file:0x%llX \"androidboot.vbmeta.device_state\"\n",
               i + 16, xc, (unsigned long long)off2);

        // Patch 第一对：复制第二对编码，寄存器改为 Xa
        uint32_t new_adrp = adrp_with_rd(i2, xa);
        uint32_t new_add  = add_with_reg(i3, xa);

        printf("  Patch pair-0: ADRP %08X->%08X, ADD %08X->%08X\n",
               i0, new_adrp, i1, new_add);

        write_instr(buffer, (int)(i + 0), new_adrp);
        write_instr(buffer, (int)(i + 4), new_add);

        patched++;
        i += 20;
    }

    if (patched == 0)
        printf("ADRL triple not found\n");
    else
        printf("ADRL patch applied: %d location(s)\n", patched);

    return patched;
}
int patch_adrl_unlocked_to_locked_verify(char* buffer, size_t size, uint64_t load_base = 0) {
    if (size < 24) return 0;

    int patched = 0;

    for (size_t i = 0; i <= size - 24; i += 4) {
        uint32_t i0 = read_instr(buffer, (int)(i +  0));
        uint32_t i1 = read_instr(buffer, (int)(i +  4));
        uint32_t i2 = read_instr(buffer, (int)(i +  8));
        uint32_t i3 = read_instr(buffer, (int)(i + 12));
        uint32_t i4 = read_instr(buffer, (int)(i + 16));
        uint32_t i5 = read_instr(buffer, (int)(i + 20));

        // 对 0: ADRP Xa + ADD Xa, Xa
        uint8_t xa = 0, rd1 = 0, rn1 = 0; uint32_t imm1 = 0;
        if (!is_adrp(i0, &xa))                     continue;
        if (!is_add_imm_x(i1, &rd1, &rn1, &imm1))  continue;
        if (rd1 != xa || rn1 != xa)                continue;

        // 对 1: ADRP Xb + ADD Xb, Xb
        uint8_t xb = 0, rd3 = 0, rn3 = 0; uint32_t imm3 = 0;
        if (!is_adrp(i2, &xb))                     continue;
        if (!is_add_imm_x(i3, &rd3, &rn3, &imm3))  continue;
        if (rd3 != xb || rn3 != xb)                continue;

        // 对 2: ADRP Xc + ADD Xc, Xc，且 Xc == X1
        uint8_t xc = 0, rd5 = 0, rn5 = 0; uint32_t imm5 = 0;
        if (!is_adrp(i4, &xc))                     continue;
        if (!is_add_imm_x(i5, &rd5, &rn5, &imm5))  continue;
        if (rd5 != xc || rn5 != xc)                continue;

        // 寄存器两两不同
        if (xa == xb || xb == xc || xa == xc)      continue;

        // ---- 字符串校验（关键！） ----
        int64_t off0 = calc_adrl_file_offset(buffer, (int)(i +  0), load_base);
        int64_t off1 = calc_adrl_file_offset(buffer, (int)(i +  8), load_base);
        int64_t off2 = calc_adrl_file_offset(buffer, (int)(i + 16), load_base);

        if (!str_at(buffer, size, off0, "locked"))                            continue;
        if (!str_at(buffer, size, off1, "locked"))                              continue;
        if (!str_at(buffer, size, off2, "androidboot.vbmeta.device_state"))     continue;

        printf("Found ADRL triple at 0x%zX:\n", i);
        printf("  [0x%zX] ADRP+ADD X%d -> file:0x%llX \"locked\"\n",
               i,      xa, (unsigned long long)off0);
        printf("  [0x%zX] ADRP+ADD X%d -> file:0x%llX \"locked\"\n",
               i + 8,  xb, (unsigned long long)off1);
        printf("  [0x%zX] ADRP+ADD X%d  -> file:0x%llX \"androidboot.vbmeta.device_state\"\n",
               i + 16, xc, (unsigned long long)off2);
        patched++;
        i += 20;
    }

    return patched;
}
int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <input_file> <output_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned char* data = nullptr;
    size_t size = 0;
    if (read_file(argv[1], &data, &size) !=0) {
        printf("Failed to read file: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (patch_abl_gbl((char*)data, size) != 0)
        printf("Warning: Failed to patch ABL GBL\n");
    
    if (patch_adrl_unlocked_to_locked((char*)data, size) == 0)
        printf("Warning: ADRL triple not found, skipping\n");
    if (patch_adrl_unlocked_to_locked_verify((char*)data, size) == 0)
        printf("Warning: ADRL verification failed\n");
    int offset= -1;
    int8_t lock_register_num = -1;
    int num_patches = patch_abl_bootstate(
        (char*)data, size, &lock_register_num, &offset);
    if (num_patches == 0) {
        printf("Error: Failed to find/patch ABL Boot State\n");
        delete[] data; return EXIT_FAILURE;
    }
    printf("Anchor offset : 0x%X\n", offset);
    printf("Lock register : W%d\n",  (int)lock_register_num);
    printf("Boot patches: %d\n",num_patches);

    if (find_ldrB_instructio_reverse(
            (char*)data, size, offset, lock_register_num) != 0) {
        printf("Warning: Failed to patch LDRB->STRB chain for W%d\n",
               (int)lock_register_num);
    }

    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        printf("Failed to open output: %s\n", argv[2]);
        delete[] data; return EXIT_FAILURE;
    }
    if (fwrite(data, 1, size, out) != size) {
        printf("Failed to write output\n");
        fclose(out); delete[] data; return EXIT_FAILURE;
    }
    fclose(out);
    delete[] data;
    printf("Saved to %s\n", argv[2]);
    return EXIT_SUCCESS;
}