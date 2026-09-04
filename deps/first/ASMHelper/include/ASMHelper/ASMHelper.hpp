#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <ASMHelper/Common.hpp>
#include <Zydis/Zydis.h>

namespace RC::ASM
{
    struct Instruction
    {
        void* address{};
        ZydisDecodedInstruction raw{};
        std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        bool valid{false};
    };

    RC_ASM_API auto is_memory_readable(const void* ptr, size_t len) -> bool;
    RC_ASM_API auto is_executable_memory(const void* ptr, size_t len) -> bool;
    RC_ASM_API auto get_first_instruction_at_address(void* in_instruction_ptr) -> Instruction;
    RC_ASM_API auto resolve_vtable_entry(void* instance, uint32_t offset) -> void*;

    RC_ASM_API auto resolve_jmp(void* instruction_ptr) -> void*;
    RC_ASM_API auto resolve_call(void* instruction_ptr) -> void*;

    RC_ASM_API auto resolve_function_address_from_potential_jmp(void* function_ptr) -> void*;
} // namespace RC::ASM
