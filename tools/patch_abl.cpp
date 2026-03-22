#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
int read_file(const char* filename, unsigned char** data, size_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        return -1; // Failed to open file
    }

    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);

    *data = new unsigned char[*size];
    if (fread(*data, 1, *size, file) != *size) {
        delete[] *data;
        fclose(file);
        return -1; // Failed to read file
    }

    fclose(file);
    return 0; // Success
}
int patch_abl_gbl(char* buffer, size_t size) {
    // Example patch: Replace "Hello" with "World"
    char target[] = "e\0f\0i\0s\0p";
    char replacement[] = "n\0u\0l\0l\0s";

    //find the target string in the buffer
    for (size_t i = 0; i < size - sizeof(target); ++i) {
        if (memcmp(buffer + i, target, sizeof(target)) == 0) {
            memcpy(buffer + i, replacement, sizeof(replacement));
            return 0; // Patch applied successfully』
        }
    }
    return -1; // Target string not found
}
/*
?? 00 00 34 28 00 80 52
06 00 00 14 E8 ?? 40 F9
08 01 40 39 1F 01 00 71
E8 07 9F 1A 08 79 1F 53
*/
int16_t Original[]={
    -1,0x00,0x00,0x34,0x28,0x00,0x80,0x52,
    0x06,0x00,0x00,0x14,0xE8,-1,0x40,0xF9,
    0x08,0x01,0x40,0x39,0x1F,0x01,0x00,0x71,
    0xE8,0x07,0x9F,0x1A,0x08,0x79,0x1F,0x53
};
int16_t Patched[]={
    -1,-1,-1,-1,0x08,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1
};

/**
 * @return 成功修补的位点数，0 表示未找到任何匹配
 */
int patch_abl_bootstate(char* buffer, size_t size, int8_t *lock_register_num,int* offset) {
    size_t pattern_len = sizeof(Original) / sizeof(int16_t);
    int patched_count = 0;

    if (size < pattern_len) return 0;

    for (size_t i = 0; i <= size - pattern_len; ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern_len; ++j) {
            if (Original[j] != -1 &&
                (unsigned char)buffer[i + j] != (unsigned char)Original[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            *lock_register_num = *(int8_t*)(&buffer[i]); // 记录锁状态寄存器的值
            *lock_register_num &=0x1F;
            *offset = i;
            for (size_t j = 0; j < pattern_len; ++j) {
                if (Patched[j] != -1) {
                    buffer[i + j] = (char)Patched[j];
                }
            }
            patched_count++;
            i += pattern_len - 1; // 跳过已修补区域，避免重叠匹配
        }
    }
    return patched_count;
}

bool is_ldrb(const char* buffer, size_t offset) {
    uint32_t instr =
        (uint8_t)buffer[offset]                  |
        ((uint8_t)buffer[offset + 1] << 8)       |
        ((uint8_t)buffer[offset + 2] << 16)      |
        ((uint8_t)buffer[offset + 3] << 24);

    return (instr & 0xFFC00000) == 0x39400000;
}
int8_t dump_register_from_LDRB(const char* instr) {
    int8_t first_byte = (int8_t)instr[0]; // 指令第一个字节（小端序）
    int8_t rt = first_byte & 0x1F;
    return (int8_t)rt; // 返回寄存器编号
}
// 检查 size=00, V=0, opc=00 (store), 基础形式
bool is_strb(const char* buffer, size_t offset) {
    uint32_t instr =
        (uint8_t)buffer[offset]             |
        ((uint8_t)buffer[offset + 1] << 8)  |
        ((uint8_t)buffer[offset + 2] << 16) |
        ((uint8_t)buffer[offset + 3] << 24);

    // STRB unsigned offset: bits[31:24]=0x39, bits[23:22]=00
    if ((instr & 0xFFC00000) == 0x39000000) return true;

    // STRB pre/post/unscaled: bits[31:21]=00111000000, bits[11:10]!=10(那是LDURB)
    if ((instr & 0xFFE00C00) == 0x38000000) return true; // post-index
    if ((instr & 0xFFE00C00) == 0x38000C00) return true; // pre-index

    return false;
}
#define MAX 0x1000
#define function_start 0xd503233f
bool is_function_start(const char* buffer, size_t offset) {
    uint32_t instr =
        (uint8_t)buffer[offset]             |
        ((uint8_t)buffer[offset + 1] << 8)  |
        ((uint8_t)buffer[offset + 2] << 16) |
        ((uint8_t)buffer[offset + 3] << 24);

    return instr == function_start;
}
int find_strb_inst_next(char* buffer, size_t size, int8_t target_register) {
    int now_offset = size; // 从第一个指令开始检查
    while (1) {
        if (is_strb(buffer, now_offset)) {
            if (dump_register_from_LDRB(buffer + now_offset) == target_register) { // 检查是否是访问 W{target_register} 寄存器
                printf("Found STRB instruction at offset: 0x%X\n", now_offset); //text:00000000000191C0                 STRB            W?, [SP,#0x660+var_600]
                printf("Instruction bytes: %02X %02X %02X %02X\n", (unsigned char)buffer[now_offset], (unsigned char)buffer[now_offset + 1], (unsigned char)buffer[now_offset + 2], (unsigned char)buffer[now_offset + 3]); 
                buffer[now_offset]|=31; // 将寄存器编号修改为 WZR/XZR
                printf("Instruction bytes: %02X %02X %02X %02X\n", (unsigned char)buffer[now_offset], (unsigned char)buffer[now_offset + 1], (unsigned char)buffer[now_offset + 2], (unsigned char)buffer[now_offset + 3]);
                return 0; // 找到目标 STRB 指令，返回0
            }
        }
        now_offset += 4; // ARM指令长度为4字节
    }
    
    return -1; // 未找到STRB指令
}
int find_ldrB_instructio_reverse(char* buffer, size_t size, int8_t target_register) {
    int now_offset = size - 4; // 从最后一个指令开始检查
    while(1){
        if (is_function_start(buffer, now_offset)) {
            printf("Reached function start at offset: 0x%X, stop searching for LDRB\n", now_offset);
            break; // 到达函数开始，停止搜索
        }
        if (is_ldrb(buffer , now_offset)) {

            if (dump_register_from_LDRB(buffer + now_offset) == target_register) { // 检查是否是访问 WZR/XZR 寄存器
                //MOV WXX #1
                printf("Found LDRB instruction at offset: 0x%X\n", now_offset);
                printf("Instruction bytes: %02X %02X %02X %02X\n", (unsigned char)buffer[now_offset], (unsigned char)buffer[now_offset + 1], (unsigned char)buffer[now_offset + 2], (unsigned char)buffer[now_offset + 3]); 
                uint32_t mov_inst = 0x52800020 | (uint8_t)target_register;

                // 小端序写入
                buffer[now_offset + 0] = (char)(mov_inst & 0xFF);
                buffer[now_offset + 1] = (char)((mov_inst >> 8) & 0xFF);
                buffer[now_offset + 2] = (char)((mov_inst >> 16) & 0xFF);
                buffer[now_offset + 3] = (char)((mov_inst >> 24) & 0xFF);
                printf("Instruction bytes: %02X %02X %02X %02X\n", (unsigned char)buffer[now_offset], (unsigned char)buffer[now_offset + 1], (unsigned char)buffer[now_offset + 2], (unsigned char)buffer[now_offset + 3]); 

                return 0; // 找到目标 LDRB 指令，返回0
            }
        }
        now_offset -= 4; // ARM指令长度为4字节
    }
    return -1; // 未找到LDRB指令
}
int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <input_file> <output_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned char* data = nullptr;
    size_t size = 0;

    if (read_file(argv[1], &data, &size) != 0) {
        printf("Failed to read file: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (patch_abl_gbl((char*)data, size) != 0) {
        printf("Failed to patch ABL GBL\n");
    }
    int offset=-1;
    int8_t lock_register_num = -1;
    int num_patches = patch_abl_bootstate((char*)data, size,&lock_register_num,&offset);
     if (num_patches == 0) {
        printf("Warning:Failed to patch ABL Boot State\n");
        delete[] data;
        return EXIT_FAILURE;
    }
    printf("OFFSET: 0x%X\n", offset);
    printf("Original lock register number W%d\n", (int)lock_register_num);
    printf("Patching completed successfully.\n");
    printf("Number of Boot State patches applied: %d\n", num_patches);
    if (find_strb_inst_next((char*)data, offset, lock_register_num) == -1) {//important
        printf("Failed to find STRB instruction accessing W%d\n", (int)lock_register_num);
        delete[] data;
        return EXIT_FAILURE;
    }
    //remove LSB to get the lock register number
    if (find_ldrB_instructio_reverse((char*)data, offset, lock_register_num) != 0) {//not important, just for better performance, if failed to find LDRB instruction, it doesn't matter, just print a warning
        printf("Failed to find LDRB instruction accessing W%d\n", (int)lock_register_num);
        fwrite(data, 1, size, stdout); // 输出修补后的数据到标准输出
        printf("Warning: Failed to find LDRB instruction accessing W%d, you can not lock the bl\n", (int)lock_register_num);
        delete[] data;
        return EXIT_FAILURE;
    }
    FILE* output_file = fopen(argv[2], "wb");
    if (!output_file) {
        printf("Failed to open output file: %s\n", argv[2]);
        delete[] data;
        return EXIT_FAILURE;
    }

    fwrite(data, 1, size, output_file);
    fclose(output_file);
    delete[] data;

    printf("Patch applied successfully and saved to %s\n", argv[2]);
    return EXIT_SUCCESS;
}