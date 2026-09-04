#include <bit>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>

#include <ASMHelper/ASMHelper.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Zydis/Zydis.h>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#else
#define NOMINMAX
#include <Windows.h>
#endif

namespace RC::ASM
{
    namespace
    {
        constexpr size_t kMaxDecodeBytes = 16;
        constexpr int kMaxJmpResolveDepth = 16;

        auto resolve_function_address_impl(void* function_ptr, int depth) -> void*;
    } // namespace

    auto is_memory_readable(const void* ptr, size_t len) -> bool
    {
        if (!ptr || len == 0)
        {
            return false;
        }
        const auto start = std::bit_cast<uintptr_t>(ptr);
        const auto end = start + len;
        // Overflow or null-page wrap.
        if (end < start || end <= 4096)
        {
            return false;
        }

#ifdef __linux__
        long page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size <= 0)
        {
            page_size = 4096;
        }
        const auto page_mask = static_cast<uintptr_t>(page_size - 1);
        const auto aligned_start = start & ~page_mask;
        // Round end up to page boundary (careful about overflow, already checked).
        const auto aligned_end = (end + page_mask) & ~page_mask;
        for (auto page = aligned_start; page < aligned_end; page += static_cast<uintptr_t>(page_size))
        {
            // MS_ASYNC performs no writes; it only faults (ENOMEM) when unmapped.
            if (::msync(std::bit_cast<void*>(page), static_cast<size_t>(page_size), MS_ASYNC) != 0)
            {
                return false;
            }
        }
        return true;
#else
        SIZE_T remaining = len;
        auto cursor = static_cast<const uint8_t*>(ptr);
        while (remaining > 0)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0)
            {
                return false;
            }
            if (mbi.State != MEM_COMMIT)
            {
                return false;
            }
            if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            {
                return false;
            }
            const auto region_start = std::bit_cast<uintptr_t>(mbi.BaseAddress);
            const auto region_end = region_start + mbi.RegionSize;
            const auto cursor_addr = std::bit_cast<uintptr_t>(cursor);
            if (cursor_addr < region_start || cursor_addr >= region_end)
            {
                return false;
            }
            const auto available = region_end - cursor_addr;
            const auto step = available < remaining ? available : remaining;
            remaining -= step;
            cursor += step;
        }
        return true;
#endif
    }

    auto is_executable_memory(const void* ptr, size_t len) -> bool
    {
        if (!is_memory_readable(ptr, len))
        {
            return false;
        }
#ifdef __linux__
        // Parse /proc/self/maps and require the whole range to sit inside
        // readable + executable mappings. Heap/stack (rw--) are readable but
        // must never be decoded as code; returning false there prevents
        // garbage JMP targets from being followed.
        const auto start = std::bit_cast<uintptr_t>(ptr);
        const auto end = start + len;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps)
        {
            // Fall back to readability when maps are unavailable.
            return true;
        }
        bool fully_covered = false;
        auto cursor = start;
        char line[512]{};
        // Single pass: maps are sorted, so walk them in order.
        // Re-scan from the top for each page-sized step keeps this simple and
        // init-time only (a few hundred calls at most).
        while (cursor < end)
        {
            std::rewind(maps);
            bool found = false;
            while (std::fgets(line, sizeof(line), maps))
            {
                uintptr_t map_start{};
                uintptr_t map_end{};
                char perms[8]{};
                if (std::sscanf(line, "%lx-%lx %7s", &map_start, &map_end, perms) != 3)
                {
                    continue;
                }
                if (cursor >= map_start && cursor < map_end)
                {
                    // Need r-x (readable + executable). vsyscall/vdso are r-x too.
                    if (perms[0] != 'r' || perms[2] != 'x')
                    {
                        std::fclose(maps);
                        return false;
                    }
                    cursor = map_end < end ? map_end : end;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                std::fclose(maps);
                return false;
            }
        }
        std::fclose(maps);
        fully_covered = (cursor >= end);
        return fully_covered;
#else
        auto cursor = static_cast<const uint8_t*>(ptr);
        SIZE_T remaining = len;
        while (remaining > 0)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0)
            {
                return false;
            }
            if (mbi.State != MEM_COMMIT)
            {
                return false;
            }
            const bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            if (!executable)
            {
                return false;
            }
            const auto region_start = std::bit_cast<uintptr_t>(mbi.BaseAddress);
            const auto region_end = region_start + mbi.RegionSize;
            const auto cursor_addr = std::bit_cast<uintptr_t>(cursor);
            if (cursor_addr < region_start || cursor_addr >= region_end)
            {
                return false;
            }
            const auto available = region_end - cursor_addr;
            const auto step = available < remaining ? available : remaining;
            remaining -= step;
            cursor += step;
        }
        return true;
#endif
    }

    auto get_first_instruction_at_address(void* in_instruction_ptr) -> Instruction
    {
        Instruction result{};
        result.address = in_instruction_ptr;

        if (!in_instruction_ptr)
        {
            return result;
        }
        // Never let Zydis touch wild memory directly. Validate first, then
        // decode from an aligned stack copy so odd/unmapped pointers and
        // page-boundary crossings cannot fault inside the decoder.
        if (!is_memory_readable(in_instruction_ptr, kMaxDecodeBytes))
        {
            return result;
        }
        uint8_t code_copy[kMaxDecodeBytes]{};
        std::memcpy(code_copy, in_instruction_ptr, kMaxDecodeBytes);

        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        ZydisDecodedInstruction decoded{};
        // Decode from the aligned copy; runtime address stays the original so
        // RIP-relative calculations remain correct.
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code_copy, kMaxDecodeBytes, &decoded, result.operands.data())))
        {
            return result;
        }
        if (decoded.length == 0 || decoded.length > kMaxDecodeBytes)
        {
            return result;
        }
        result.raw = decoded;
        result.valid = true;
        return result;
    }

    auto resolve_absolute_address(void* in_instruction_ptr) -> void*
    {
        if (!in_instruction_ptr)
        {
            return nullptr;
        }
        // Only resolve actual control-flow instructions; resolving arbitrary
        // opcodes (e.g. LEA) would yield bogus addresses.
        auto instruction = get_first_instruction_at_address(in_instruction_ptr);
        if (!instruction.valid)
        {
            return nullptr;
        }
        const auto mnemonic = instruction.raw.mnemonic;
        if (mnemonic != ZYDIS_MNEMONIC_JMP && mnemonic != ZYDIS_MNEMONIC_CALL)
        {
            return nullptr;
        }
        ZyanU64 resolved_address{};
        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction.raw, &instruction.operands[0], std::bit_cast<ZyanU64>(in_instruction_ptr), &resolved_address)))
        {
            auto target = std::bit_cast<void*>(resolved_address);
            // The JMP/CALL target must itself be readable code; otherwise the
            // vtable slot held garbage and following it would crash.
            if (!target || !is_memory_readable(target, 1))
            {
                return nullptr;
            }
            return target;
        }
        else
        {
            return nullptr;
        }
    }

    auto resolve_jmp(void* in_instruction_ptr) -> void*
    {
        return resolve_absolute_address(in_instruction_ptr);
    }

    auto resolve_call(void* in_instruction_ptr) -> void*
    {
        return resolve_absolute_address(in_instruction_ptr);
    }

    namespace
    {
        auto resolve_function_address_impl(void* function_ptr, int depth) -> void*
        {
            if (!function_ptr || depth > kMaxJmpResolveDepth)
            {
                return nullptr;
            }
            // Reject non-code pointers (heap/stack garbage from torn vtable
            // reads or past-the-end slots) before decoding.
            if (!is_executable_memory(function_ptr, 1))
            {
                return nullptr;
            }
            auto instruction = get_first_instruction_at_address(function_ptr);
            if (!instruction.valid)
            {
                return nullptr;
            }
            if (instruction.raw.mnemonic == ZYDIS_MNEMONIC_JMP || instruction.raw.mnemonic == ZYDIS_MNEMONIC_CALL)
            {
                if (auto resolved_address = resolve_jmp(instruction.address); resolved_address)
                {
                    return resolve_function_address_impl(resolved_address, depth + 1);
                }
                else
                {
                    Output::send<LogLevel::Warning>(STR("Was unable to resolve JMP instruction @ {}\n"), instruction.address);
                    return nullptr;
                }
            }
            else
            {
                return function_ptr;
            }
        }
    } // namespace

    auto resolve_function_address_from_potential_jmp(void* function_ptr) -> void*
    {
        return resolve_function_address_impl(function_ptr, 0);
    }

    auto resolve_vtable_entry(void* instance, uint32_t offset) -> void*
    {
        if (!instance)
        {
            return nullptr;
        }
        // Guard the instance->vtable read itself: during level loads the game
        // thread can be (re)constructing the object concurrently, so the
        // vtable pointer may be torn/null momentarily.
        if (!is_memory_readable(instance, sizeof(void*)))
        {
            return nullptr;
        }
        auto vtable = *std::bit_cast<uint8_t**>(instance);
        if (!vtable)
        {
            return nullptr;
        }
        // Guard vtable+offset before dereferencing: wrong layouts or
        // past-the-end diagnostic reads must not fault.
        // Check readability of the slot without overflowing.
        const auto slot_addr = std::bit_cast<uintptr_t>(vtable) + offset;
        if (slot_addr < std::bit_cast<uintptr_t>(vtable))
        {
            return nullptr;
        }
        if (!is_memory_readable(std::bit_cast<void*>(slot_addr), sizeof(void*)))
        {
            return nullptr;
        }
        auto entry = *std::bit_cast<void**>(std::bit_cast<uint8_t*>(vtable) + offset);
        if (!entry)
        {
            return nullptr;
        }
        return resolve_function_address_from_potential_jmp(entry);
    }
} // namespace RC::ASM
