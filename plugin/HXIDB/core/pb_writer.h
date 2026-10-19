// Minimal protobuf encoder matching proto/hxidb.proto (v1). No dependencies.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hxidb {

using Bytes = std::vector<uint8_t>;

/**
 * PbWriter - builds a protobuf message byte by byte.
 *
 * U/S/B/Str/Raw write one field each (zero and empty values are left out like proto3 does), Msg writes an embedded
 * message, and Data() returns the bytes written so far.
 */
class PbWriter
{
    Bytes m_b;

public:
    void Varint(uint64_t v)
    {
        while (v >= 0x80) { m_b.push_back((uint8_t)(v | 0x80)); v >>= 7; }
        m_b.push_back((uint8_t)v);
    }
    void Tag(uint32_t field, uint32_t wire) { Varint((uint64_t)field << 3 | wire); }

    // proto3: zero/empty scalars are omitted.
    void U(uint32_t field, uint64_t v) { if (v) { Tag(field, 0); Varint(v); } }
    void S(uint32_t field, int32_t v)  { if (v) { Tag(field, 0); Varint(((uint32_t)v << 1) ^ (uint32_t)(v >> 31)); } }  // sint32 zigzag
    void B(uint32_t field, bool v)     { if (v) { Tag(field, 0); Varint(1); } }
    void Raw(uint32_t field, const uint8_t* p, size_t n) { if (n) { Tag(field, 2); Varint(n); m_b.insert(m_b.end(), p, p + n); } }
    void Raw(uint32_t field, const Bytes& v) { Raw(field, v.data(), v.size()); }
    void Str(uint32_t field, const std::string& v) { Raw(field, (const uint8_t*)v.data(), v.size()); }
    // Embedded message: always written (a repeated element may legitimately be empty).
    void Msg(uint32_t field, const Bytes& v) { Tag(field, 2); Varint(v.size()); m_b.insert(m_b.end(), v.begin(), v.end()); }

    const Bytes& Data() const { return m_b; }
};

} // namespace hxidb
