#include "vm.h"
#include "vm_chunk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Helper to get instruction length for relocation
static int get_instruction_len(uint8_t op);

static uint32_t read_uint32_at(const uint8_t* code, size_t offset) {
    return ((uint32_t)code[offset] << 24) |
           ((uint32_t)code[offset + 1] << 16) |
           ((uint32_t)code[offset + 2] << 8) |
           ((uint32_t)code[offset + 3]);
}

static void write_uint32_at(uint8_t* code, size_t offset, uint32_t value) {
    code[offset] = (uint8_t)((value >> 24) & 0xFF);
    code[offset + 1] = (uint8_t)((value >> 16) & 0xFF);
    code[offset + 2] = (uint8_t)((value >> 8) & 0xFF);
    code[offset + 3] = (uint8_t)(value & 0xFF);
}

static int get_instruction_len(uint8_t op) {
    switch (op) {
        case OP_CONSTANT: return 5;
        case OP_LOG: return 1;
        case OP_LOG_S: return 1;
        case OP_CALL: return 6;
        case OP_RETURN: return 2;
        case OP_GET_LOCAL: return 5;
        case OP_SET_LOCAL: return 5;
        case OP_ALLOC_LOCAL: return 5;
        case OP_POP: return 1;
        case OP_JUMP: return 5;
        case OP_JUMP_IF_FALSE: return 5;
        case OP_ADD: return 1;
        case OP_SUB: return 1;
        case OP_MUL: return 1;
        case OP_DIV: return 1;
        case OP_MOD: return 1;
        case OP_NEG: return 1;
        case OP_LT: return 1;
        case OP_LE: return 1;
        case OP_GT: return 1;
        case OP_GE: return 1;
        case OP_EQ: return 1;
        case OP_NE: return 1;
        case OP_NOT: return 1;
        case OP_NATIVE_CALL: return 6;
        case OP_GET_FIELD: return 5;
        case OP_SET_FIELD: return 5;
        case OP_CONSTRUCT: return 5;
        case OP_BIND_LOCAL: return 5;
        case OP_BIND_FIELD: return 5;
        case OP_REF_VIEW: return 1;
        case OP_REF_MOD: return 1;
        case OP_VIEW_LOCAL: return 5;
        case OP_MOD_LOCAL: return 5;
        case OP_VIEW_FIELD: return 5;
        case OP_MOD_FIELD: return 5;
        case OP_GET_GLOBAL: return 5;
        case OP_SET_GLOBAL: return 5;
        case OP_GET_GLOBAL_INIT_BIT: return 5;
        case OP_SET_GLOBAL_INIT_BIT: return 5;
        case OP_SET_LOCAL_FIELD: return 9; // OP + slot(4) + field(4)
        case OP_DUP: return 1;
        default: return 1;
    }
}

bool vm_hot_patch(VM* vm, Chunk* new_chunk) {
    if (!vm || !new_chunk) return false;
    
    Chunk* old_chunk = vm->chunk;
    uint8_t* old_code_start = old_chunk->code;
    size_t code_offset = old_chunk->code_count;
    size_t const_offset = old_chunk->constants_count;
    
    // Save IP offset
    size_t ip_offset = 0;
    if (vm->ip >= old_chunk->code && vm->ip < old_chunk->code + old_chunk->code_count) {
        ip_offset = vm->ip - old_chunk->code;
    }
    
    // 2. Append constants
    for (size_t i = 0; i < new_chunk->constants_count; ++i) {
        chunk_add_constant(old_chunk, value_copy(&new_chunk->constants[i]));
    }
    
    // 3. Append code (relocated)
    for (size_t i = 0; i < new_chunk->code_count; ++i) {
        chunk_write(old_chunk, new_chunk->code[i], new_chunk->lines[i]);
    }
    
    // Restore IP (in case realloc moved it)
    if (ip_offset > 0 || vm->ip == old_code_start) {
        vm->ip = old_chunk->code + ip_offset;
    }

    // Update Call Stack Return IPs if realloc happened
    if (old_chunk->code != old_code_start) {
        printf("[hot-patch] Code buffer moved. Relocating stack frames...\n");
        ptrdiff_t diff = old_chunk->code - old_code_start;
        for (size_t i = 0; i < vm->call_stack_top; ++i) {
            CallFrame* frame = &vm->call_stack[i];
            if (frame->return_ip >= old_code_start && frame->return_ip < old_code_start + code_offset) { // Check if within old range
                 frame->return_ip += diff;
            }
        }
    }
    
    // 4. Relocate instructions in the appended block
    uint8_t* code_base = old_chunk->code + code_offset;
    size_t cursor = 0;
    while (cursor < new_chunk->code_count) {
        uint8_t op = code_base[cursor];
        int len = get_instruction_len(op);
        
        if (cursor + len > new_chunk->code_count) {
            printf("[hot-patch] Error: Instruction relocation overflow at cursor %zu\n", cursor);
            break;
        }

        if (op == OP_CONSTANT) {
            uint32_t idx = read_uint32_at(code_base, cursor + 1);
            idx += (uint32_t)const_offset;
            write_uint32_at(code_base, cursor + 1, idx);
        } else if (op == OP_NATIVE_CALL) {
            uint32_t idx = read_uint32_at(code_base, cursor + 1);
            idx += (uint32_t)const_offset;
            write_uint32_at(code_base, cursor + 1, idx);
        } else if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_CALL) {
            uint32_t target = read_uint32_at(code_base, cursor + 1);
            target += (uint32_t)code_offset;
            write_uint32_at(code_base, cursor + 1, target);
        }
        cursor += len;
    }
    
    // 5. Install Trampolines and update function registry
    int patched_count = 0;
    for (size_t i = 0; i < new_chunk->functions_count; ++i) {
        FunctionDebugInfo* new_fn = &new_chunk->functions[i];
        size_t new_addr = code_offset + new_fn->offset;
        
        // Find old function to patch
        for (size_t j = 0; j < old_chunk->functions_count; ++j) {
            FunctionDebugInfo* old_fn = &old_chunk->functions[j];
            if (strcmp(old_fn->name, new_fn->name) == 0) {
                // Found match! Write trampoline at original offset.
                // This ensures existing calls to the original address are redirected.
                old_chunk->code[old_fn->offset] = OP_JUMP;
                write_uint32_at(old_chunk->code, old_fn->offset + 1, (uint32_t)new_addr);
                
                printf("[hot-patch] Patched function: %s -> offset %zu\n", old_fn->name, new_addr);
                patched_count++;
                break;
            }
        }
        
        // Unconditionally add new function info so future patches can find it at its new location
        chunk_add_function_info(old_chunk, new_fn->name, new_addr);
    }
    
    printf("[hot-patch] Applied %d patches. Code size: %zu -> %zu\n", 
           patched_count, code_offset, old_chunk->code_count);
           
    return true;
}
