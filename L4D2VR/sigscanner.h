#pragma once
#include <Windows.h>
#include <psapi.h>
#include <vector>
#include <sstream>
#include <limits>
#include <cstdint>
#include <string>

class SigScanner
{
public:
	// Returns 0 if current offset matches, -1 if no matches found.
	// A value > 0 is the new offset.
	static int VerifyOffset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
	{
		int matches = 0;
		return VerifyOffset(moduleName, currentOffset, signature, sigOffset, matches);
	}

	// As above; matches = how many places in the module match the signature
	// when the known offset did not (a relocation picked among them).
	static int VerifyOffset(std::string moduleName, int currentOffset, std::string signature, int sigOffset, int& matches)
	{
		matches = 0;
		HMODULE hModule = GetModuleHandle(moduleName.c_str());
		if (!hModule)
			return -1;

		// 2.0.11 (review B47): a failed query left base and size uninitialised.
		MODULEINFO moduleInfo{};
		if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)) ||
			!moduleInfo.lpBaseOfDll || moduleInfo.SizeOfImage == 0)
			return -1;

		uint8_t* bytes = (uint8_t*)moduleInfo.lpBaseOfDll;

		std::vector<int> pattern;
		std::stringstream ss(signature);
		std::string sigByte;
		while (ss >> sigByte)
		{
			if (sigByte == "?" || sigByte == "??")
				pattern.push_back(-1);
			else
				pattern.push_back(strtoul(sigByte.c_str(), NULL, 16));
		}

		const int patternLen = (int)pattern.size();
		if (patternLen <= 0 || patternLen > (int)moduleInfo.SizeOfImage)
			return -1;

		// Check if current offset is good when a known offset exists. Only
		// inside the image: after an update that made the module smaller the
		// old offset pointed past the mapping and the probe crashed the game (B47).
		const long long knownStart = (long long)currentOffset - sigOffset;
		if (currentOffset > 0 && knownStart >= 0 && knownStart + patternLen <= (long long)moduleInfo.SizeOfImage)
		{
			bool offsetMatchesSig = true;
			for (int i = 0; i < patternLen; ++i)
			{
				if ((bytes[currentOffset - sigOffset + i] != pattern[i]) && (pattern[i] != -1))
				{
					offsetMatchesSig = false;
					break;
				}
			}

			if (offsetMatchesSig)
				return 0;
		}

		// Scan the dll for new offset.
		//
		// IMPORTANT: Many of our signatures are "short" (prologue-only) and can match multiple sites.
		// If we have a previous known offset, pick the closest match to reduce false positives.
		int bestOffset = -1;
		int bestDist = std::numeric_limits<int>::max();

		for (int i = 0; i <= (int)moduleInfo.SizeOfImage - patternLen; ++i)
		{
			bool found = true;
			for (int j = 0; j < patternLen; ++j)
			{
				if ((bytes[i + j] != pattern[j]) && (pattern[j] != -1))
				{
					found = false;
					break;
				}
			}
			if (!found)
				continue;

			const int candidate = i + sigOffset;
			if (candidate <= 0)
				continue;
			++matches;

			// No prior offset: keep old behavior (first match).
			if (currentOffset <= 0)
				return candidate;

			int dist = candidate - currentOffset;
			if (dist < 0)
				dist = -dist;

			if (dist < bestDist)
			{
				bestDist = dist;
				bestOffset = candidate;
			}
		}

		return bestOffset;
	}

	// For a hook at a fixed offset with no signature: true when the address
	// is inside an executable section and looks like a function start (after
	// int3/nop padding or a ret), so a rebuilt DLL is not patched blindly (B39).
	static bool LooksLikeFunctionStart(HMODULE module, uintptr_t offset)
	{
		if (!module || offset < 16)
			return false;
		MODULEINFO info{};
		if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) || offset + 16 > info.SizeOfImage)
			return false;
		const uint8_t* base = reinterpret_cast<const uint8_t*>(info.lpBaseOfDll);
		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		bool executable = false;
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			const uintptr_t start = section->VirtualAddress;
			const uintptr_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
			if (offset >= start && offset < start + size)
			{
				executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
				break;
			}
		}
		if (!executable)
			return false;
		const uint8_t before = base[offset - 1];
		const bool framePrologue = base[offset] == 0x55 && base[offset + 1] == 0x8B && base[offset + 2] == 0xEC;
		return framePrologue || before == 0xCC || before == 0x90 || before == 0xC3 || base[offset - 3] == 0xC2;
	}
};
