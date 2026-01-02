#include "Memory.h"
#include <format>
#include <Psapi.h>
#include <Zydis/Zydis.h>
#include <optional>

// Fast hex char to byte conversion
static constexpr uint8_t HexToByte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// Optimized pattern parser
// Returns vector of {byte, is_wildcard}
static std::vector<std::pair<uint8_t, bool>> ParsePattern(const char* szPattern)
{
    std::vector<std::pair<uint8_t, bool>> result;
    result.reserve(strlen(szPattern) / 2);

    for (const char* pCurrent = szPattern; *pCurrent; ++pCurrent)
    {
        if (*pCurrent == ' ') continue;

        if (*pCurrent == '?')
        {
            result.emplace_back(0, true);
            if (*(pCurrent + 1) == '?') pCurrent++; // Handle ??
        }
        else
        {
            uint8_t byte = (HexToByte(*pCurrent) << 4) | HexToByte(*(pCurrent + 1));
            result.emplace_back(byte, false);
            pCurrent++;
        }
    }
    return result;
}

uintptr_t CMemory::FindSignature(const char* szModule, const char* szPattern)
{
    const auto hModule = GetModuleHandleA(szModule);
    if (!hModule) return 0x0;

    MODULEINFO lpModuleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &lpModuleInfo, sizeof(MODULEINFO)))
        return 0x0;

    const auto dwImageSize = lpModuleInfo.SizeOfImage;
    if (!dwImageSize) return 0x0;

    const auto vPattern = ParsePattern(szPattern);
    const byte* pImageBytes = reinterpret_cast<byte*>(hModule);
    const size_t patternSize = vPattern.size();

    // Simple implementation - For production AVX2 is recommended for large scans
    for (size_t i = 0; i < dwImageSize - patternSize; ++i)
    {
        bool bFound = true;
        for (size_t j = 0; j < patternSize; ++j)
        {
            if (!vPattern[j].second && pImageBytes[i + j] != vPattern[j].first)
            {
                bFound = false;
                break;
            }
        }
        if (bFound)
            return uintptr_t(&pImageBytes[i]);
    }

    return 0x0;
}

std::string CMemory::GetModuleName(uintptr_t uAddress)
{
    HMODULE hModule;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)uAddress, &hModule))
    {
        char cModuleName[MAX_PATH];
        if (GetModuleBaseNameA(GetCurrentProcess(), hModule, cModuleName, MAX_PATH))
            return cModuleName;
    }
    return "Unknown";
}

// ... (Rest of FindSignatureAtAddress can utilize ParsePattern for optimization)

// AI failed me so i had to learn this shit by myself
// Optimized Zydis signature generation
std::string CMemory::GenerateSignatureAtAddress(uintptr_t uAddress, size_t maxLength)
{
    std::string sPattern;
    std::string sModule = GetModuleName(uAddress);
    
    HMODULE hMod = GetModuleHandleA(sModule.c_str());
    if (!hMod) return {};

    MODULEINFO lpModuleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &lpModuleInfo, sizeof(MODULEINFO))) return {};
    
    uintptr_t uMinAddr = (uintptr_t)hMod;
    uintptr_t uMaxAddr = uMinAddr + lpModuleInfo.SizeOfImage;

    ZydisDecoder tDecoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&tDecoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return {};

    uintptr_t uLastFound = uMinAddr;
    uintptr_t uCurrentAddr = uAddress;
    
    // Store bytes and wildcards
    std::vector<std::pair<uint8_t, bool>> vSignatureBytes;

    while (uCurrentAddr - uAddress < maxLength && uCurrentAddr < uMaxAddr)
    {
        ZydisDecodedInstruction tInstruction;
        if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&tDecoder, nullptr, (void*)(uCurrentAddr), 15, &tInstruction)))
            break;

        // Calculate offset for relative addressing to mask
        // If an instruction has relative displacement, we usually want to wildcard those bytes
        byte uOffset = tInstruction.raw.disp.offset + (tInstruction.raw.imm ? tInstruction.raw.imm->offset : 0);
        
        for (int i = 0; i < tInstruction.length; i++)
        {
            // Mask relative offsets (dynamic memory addresses)
            bool isWildcard = (uOffset != 0 && i >= uOffset);
            vSignatureBytes.push_back({ *(byte*)(uCurrentAddr + i), isWildcard });
        }

        // Construct string pattern for verification
        std::string sTempPattern;
        for (const auto& [val, wildcard] : vSignatureBytes)
            sTempPattern.append(wildcard ? "? " : std::format("{:02X} ", val));
        
        if (!sTempPattern.empty()) sTempPattern.pop_back();

        // Verify uniqueness
        bool bFoundAtCurrent = false;
        FindSignatureAtAddress(uAddress, sTempPattern.c_str(), uAddress, &bFoundAtCurrent);

        if (bFoundAtCurrent)
        {
            // Check if it exists elsewhere
            uintptr_t uFound = FindSignatureAtAddress(uLastFound, sTempPattern.c_str(), uAddress);
            if (!uFound) {
                // Unique!
                sPattern = sTempPattern;
                break;
            }
            uLastFound = uFound; // Optimization: Skip past what we already found
        }
        else {
            break; // Should not happen if logic is correct
        }

        uCurrentAddr += tInstruction.length;
    }

    return sPattern;
}

// ... rest of file ...
