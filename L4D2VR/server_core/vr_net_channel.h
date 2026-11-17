#pragma once

// Server -> client string commands in the *unreliable* stream of a client's
// net channel (2.0.12, Opus voice relay protocol 2). std-only: the bit layout,
// the vtable slots and the code-shape checks; the hosts (Linux plugin, Windows
// plugin, modded listen host) make the calls.
//
// IVEngineServer::ClientCommand sends NET_StringCmd reliably: every voice frame
// waited for the previous reliable block's ack, so frames came in clumps and a
// lost datagram stalled all later ones. Here the same message is written as
// raw bits with INetChannel::SendData(bf_write&, bReliable = false): it goes
// out with the next datagram or is lost, and the client processes it exactly
// like the reliable one (CBaseClientState::ProcessStringCmd -> Cbuf).
//
// Checked against L4D2 build 2.2.4.x (2026):
//  * engine_srv.so (GCC, symbols): NET_StringCmd::GetType() == 5,
//    NET_StringCmd::WriteToBuffer writes the type in 6 bits then the string;
//    CVEngineServer::GetPlayerNetInfo is IVEngineServer slot 21 and returns
//    client->m_NetChannel; CNetChan vtable: SendData slot 43,
//    GetNumBitsWritten(bool) slot 67 (two destructor slots at 27/28).
//  * engine.dll (MSVC, RTTI): same type id and width; CNetChan::SendData slot
//    42, GetNumBitsWritten slot 66 (one deleting destructor), GetPlayerNetInfo
//    slot 21.
//  * bf_write (both): m_pData +0, m_nDataBytes +4, m_nDataBits +8,
//    m_iCurBit +0xC, m_bOverflow +0x10. SendData reads only m_pData,
//    m_iCurBit and m_bOverflow, copies the bits into the channel's own
//    stream and returns.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace l4d2vr_netchan
{
    constexpr int kNetMessageTypeBits = 6;
    constexpr unsigned kNetStringCmd = 5;
    constexpr std::size_t kMaxStringCmdChars = 1023;   // NET_StringCmd reads into 1024 bytes

    constexpr int kEngineServerGetPlayerNetInfoSlot = 21;
    constexpr int kGccSendDataSlot = 43, kMsvcSendDataSlot = 42;
    constexpr int kGccNumBitsWrittenSlot = 67, kMsvcNumBitsWrittenSlot = 66;

    // The NET_StringCmd message as bf_write writes it: LSB-first bit stream
    // (byte i holds stream bits 8i..8i+7), type in 6 bits, then the string and
    // its NUL. Returns the bits written, or -1 when `out` (bytes) is too small
    // or the text is too long. The tail of `out` up to a 4-byte boundary is
    // zeroed (bf_write::WriteBits may read whole words).
    inline int BuildStringCmd(const char* text, std::uint8_t* out, std::size_t capacity)
    {
        if (!text || !out) return -1;
        const std::size_t length = std::strlen(text);
        if (length > kMaxStringCmdChars) return -1;
        const std::size_t bits = static_cast<std::size_t>(kNetMessageTypeBits) + 8u * (length + 1u);
        const std::size_t bytes = (bits + 7u) / 8u;
        const std::size_t padded = (bytes + 3u) & ~std::size_t{ 3 };
        if (padded > capacity) return -1;
        std::memset(out, 0, padded);
        std::size_t bit = 0;
        auto put = [&](unsigned value, int count) {
            for (int i = 0; i < count; ++i, ++bit)
                if ((value >> i) & 1u) out[bit >> 3] = static_cast<std::uint8_t>(out[bit >> 3] | (1u << (bit & 7u)));
        };
        put(kNetStringCmd, kNetMessageTypeBits);
        for (std::size_t i = 0; i <= length; ++i) put(static_cast<unsigned char>(text[i]), 8);
        return static_cast<int>(bits);
    }

    // Reads back what BuildStringCmd wrote (tests).
    inline bool ParseStringCmd(const std::uint8_t* in, int bits, char* text, std::size_t capacity)
    {
        if (!in || !text || capacity == 0 || bits < kNetMessageTypeBits + 8) return false;
        int bit = 0;
        auto get = [&](int count) {
            unsigned v = 0;
            for (int i = 0; i < count; ++i, ++bit) v |= static_cast<unsigned>((in[bit >> 3] >> (bit & 7)) & 1u) << i;
            return v;
        };
        if (get(kNetMessageTypeBits) != kNetStringCmd) return false;
        std::size_t n = 0;
        while (bit + 8 <= bits)
        {
            const char c = static_cast<char>(get(8));
            if (c == '\0') { text[n] = '\0'; return true; }
            if (n + 1 >= capacity) return false;
            text[n++] = c;
        }
        return false;
    }

    inline bool Matches(const std::uint8_t* code, std::size_t available, const std::uint8_t* expected, std::size_t n)
    {
        return code && available >= n && std::memcmp(code, expected, n) == 0;
    }

    // CNetChan::SendData(bf_write&, bool), GCC (engine_srv.so):
    //   push ebp; mov ebp,esp; push edi; push esi; push ebx; sub esp,1Ch;
    //   mov ebx,[ebp+8]; mov edi,[ebp+0Ch]; mov esi,[ebp+10h]; lea eax,[ebx+98h]
    inline bool GccSendDataCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x1C, 0x8B, 0x5D, 0x08,
            0x8B, 0x7D, 0x0C, 0x8B, 0x75, 0x10, 0x8D, 0x83, 0x98, 0x00, 0x00, 0x00 };
        return Matches(code, available, k, sizeof(k));
    }

    // MSVC (engine.dll): push ebp; mov ebp,esp; push esi; mov esi,ecx; lea ecx,[esi+98h]; call
    inline bool MsvcSendDataCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8D, 0x8E, 0x98, 0x00, 0x00, 0x00, 0xE8 };
        return Matches(code, available, k, sizeof(k));
    }

    // CNetChan::GetNumBitsWritten(bool reliable), GCC: the reliable stream is
    // at +0x20, the unreliable one at +0x44 (m_iCurBit +0xC).
    //   push ebp; mov ebp,esp; mov edx,[ebp+8]; lea eax,[edx+44h]; add edx,20h; cmp byte [ebp+0Ch],0
    inline bool GccNumBitsWrittenCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x89, 0xE5, 0x8B, 0x55, 0x08, 0x8D, 0x42, 0x44, 0x83, 0xC2, 0x20,
            0x80, 0x7D, 0x0C, 0x00 };
        return Matches(code, available, k, sizeof(k));
    }

    // MSVC: push ebp; mov ebp,esp; cmp byte [ebp+8],0; je +7; mov eax,[ecx+2Ch]; pop ebp; ret 4
    inline bool MsvcNumBitsWrittenCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x8B, 0xEC, 0x80, 0x7D, 0x08, 0x00, 0x74, 0x07, 0x8B, 0x41, 0x2C,
            0x5D, 0xC2, 0x04, 0x00 };
        return Matches(code, available, k, sizeof(k));
    }

    // CVEngineServer::GetPlayerNetInfo(int), GCC: push ebp; xor eax,eax; mov ebp,esp; mov edx,[ebp+0Ch]; test edx,edx; jle
    inline bool GccGetPlayerNetInfoCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x31, 0xC0, 0x89, 0xE5, 0x8B, 0x55, 0x0C, 0x85, 0xD2, 0x7E };
        return Matches(code, available, k, sizeof(k));
    }

    // MSVC: push ebp; mov ebp,esp; mov eax,[ebp+8]; cmp eax,1; jl
    inline bool MsvcGetPlayerNetInfoCode(const std::uint8_t* code, std::size_t available)
    {
        static const std::uint8_t k[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xF8, 0x01, 0x7C };
        return Matches(code, available, k, sizeof(k));
    }

    // bf_write as SendData reads it (32-bit builds of the game).
    struct BfWrite32
    {
        std::uint32_t data;          // unsigned long* m_pData
        std::int32_t dataBytes;
        std::int32_t dataBits;
        std::int32_t curBit;         // bits written
        bool overflow;
        bool assertOnOverflow;
        std::uint8_t pad[2];
        std::uint32_t debugName;
    };
    static_assert(sizeof(BfWrite32) == 0x18, "bf_write layout");
    static_assert(offsetof(BfWrite32, curBit) == 0xC && offsetof(BfWrite32, overflow) == 0x10, "bf_write layout");

    // A bf_write over `bytes` holding `bits` bits (32-bit address of the buffer).
    inline BfWrite32 MakeBfWrite(std::uint32_t bufferAddress, int bufferBytes, int bits)
    {
        BfWrite32 w{};
        w.data = bufferAddress;
        w.dataBytes = bufferBytes;
        w.dataBits = bufferBytes * 8;
        w.curBit = bits;
        w.overflow = false;
        w.assertOnOverflow = false;
        w.debugName = 0u;
        return w;
    }

    // Sends that would not fit are refused before the call (the unreliable
    // stream drops a message that does not fit, silently).
    constexpr std::size_t kMaxMessageBytes = 1100;
}
