/*
* codec.h
* Author: Mikhail Gorbunov, RIT
* Copyright(c) 2025. All rights reserved.
*/

#pragma once

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <array>
#include <ostream>
#include <cassert>

struct ZSTD_CCtx_s;
typedef struct ZSTD_CCtx_s ZSTD_CCtx;

namespace RIT::MD
{

struct ISink
{
  virtual ~ISink() = default;
  virtual void write(const uint8_t* data, size_t n) = 0; // write block
  virtual void flush() = 0; // push buffered
  virtual void finish() = 0; // end stream/frame
};

struct OStreamSink final : ISink
{
  std::ostream& os;
  explicit OStreamSink(std::ostream& s);
  void write(const uint8_t* data, size_t n) override;
  void flush() override;
  void finish() override;
};

struct VectorSink final : ISink
{
  std::vector<uint8_t>& out;
  explicit VectorSink(std::vector<uint8_t>& v);
  void write(const uint8_t* data, size_t n) override;
  void flush() override;
  void finish() override;
};

struct RawBufferSink final : ISink
{
  uint8_t* dst = nullptr;
  size_t cap = 0;
  size_t pos = 0;

  RawBufferSink(void* p, size_t c);
  void write(const uint8_t* data, size_t n) override; // throws on overflow
  void flush() override;
  void finish() override;

  size_t size() const { return pos; }
};

struct ZstdStreamCompressor final : ISink
{
  static constexpr size_t kOutCap = 128 * 1024;

  ISink& down;
  ZSTD_CCtx* cctx = nullptr;
  int level = 3;
  std::array<uint8_t, kOutCap> out_buf{};

  explicit ZstdStreamCompressor(ISink& downstream, int lvl = 3);
  ~ZstdStreamCompressor() override;

  void write(const uint8_t* data, size_t n) override; // compress block
  void flush() override; // zstd flush
  void finish() override; // end frame
};

static constexpr uint64_t POW10[16] =
{
  1ull,
  10ull,
  100ull,
  1000ull,
  10000ull,
  100000ull,
  1000000ull,
  10000000ull,
  100000000ull,
  1000000000ull,
  10000000000ull,
  100000000000ull,
  1000000000000ull,
  10000000000000ull,
  100000000000000ull,
  1000000000000000ull,
};

static inline uint64_t zigzag_encode(int64_t v)
{
  // maps: 0->0, -1->1, 1->2, -2->3, ...
  return (uint64_t(v) << 1) ^ uint64_t(-(v < 0));
}
static inline int64_t zigzag_decode(uint64_t z)
{
  int64_t v = static_cast<int64_t>(z >> 1);
  return v ^ -static_cast<int64_t>(z & 1);
}

// ---- buffered bit writer (64 KiB), LSB-first ----
struct BufferedBitWriter
{
  static constexpr size_t kBufCap = 64 * 1024;

  ISink& sink;
  std::array<uint8_t, kBufCap> buf{};
  size_t pos = 0;
  size_t total_sz = 0;
  uint64_t acc = 0;
  unsigned bits = 0;

  explicit BufferedBitWriter(ISink& s);

  uint64_t bits_written() const
  {
    return ((total_sz + pos) << 3) + bits;
  }

  void put(uint64_t v, unsigned b)
  {
    if( !b )
      return;
    const uint64_t mask = (b == 64) ? ~0ull : ((1ull << b) - 1);
    acc |= (v & mask) << bits;
    bits += b;
    while( bits >= 8 )
    {
      write_byte(uint8_t(acc & 0xFF));
      acc >>= 8;
      bits -= 8;
    }
  }

  void align_to_byte()
  {
    if( bits )
    {
      write_byte(uint8_t(acc & 0xFF));
      acc = 0;
      bits = 0;
    }
  }

  void flush()
  {
    if( pos )
    {
      sink.write(buf.data(), pos);
      total_sz+=pos;
      pos = 0;
    }
    sink.flush();
  }

  void finish()
  {
    align_to_byte();
    if( pos )
    {
      sink.write(buf.data(), pos);
      total_sz+=pos;
      pos = 0;
    }
    sink.finish();
  }

  void put_var(uint64_t v)
  {
    while( v >= 0x80 )
    {
      put(uint8_t(v | 0x80), 8);
      v >>= 7;
    }
    put(uint8_t(v), 8);
  }
  void put_var(uint32_t v) { put_var( (uint64_t)v ); }
  void put_var(uint16_t v) { put_var( (uint64_t)v ); }
  void put_var(uint8_t v) { put_var( (uint64_t)v ); }
  template<typename T>
  void put_var(T v) = delete;

  void put_var_zero(uint64_t v)
  {
    put(v == 0, 1);
    if( v == 0 )
      return;

    put_var(v);
  }

  void put_var_sign_zero(int64_t v)
  {
    put(v == 0, 1);
    if( v == 0 )
      return;

    put_var(zigzag_encode(v));
  }

  void put_var_dec_zeros(uint64_t v)
  {
    put(v == 0, 1);
    if( v == 0 )
      return;

    unsigned k = 0;
    for( ;; )
    {
      if( v % 10 )
        break;

      v /= 10;
      ++k;

      if( k==15 )
        break;
    }

    put(k, 4);
    put_var(v);
  }

  void put_var_sign_dec_zeros(int64_t sv)
  {
    put(sv == 0, 1);
    if( sv == 0 )
      return;

    unsigned k = 0;
    for( ;; )
    {
      if( sv % 10 )
        break;

      sv /= 10;
      ++k;

      if( k==15 )
        break;
    }

    put(k, 4);
    put_var(zigzag_encode(sv));
  }

  uint64_t put_var_zero(uint64_t v, uint64_t base)
  {
    assert( v>= base );
    put_var_zero( v - base );
    return v;
  }
  uint64_t put_var(uint64_t v, uint64_t base)
  {
    assert( v>= base );
    put_var( v - base );
    return v;
  }
  uint64_t put_var_dec_zeros(uint64_t v, uint64_t base)
  {
    assert( v>= base );
    put_var_dec_zeros( v - base );
    return v;
  }
  uint64_t put_var_sign_dec_zeros(uint64_t v, uint64_t base)
  {
    put_var_sign_dec_zeros( v - base );
    return v;
  }
  int64_t put_var_sign_zero(int64_t v, int64_t base)
  {
    put_var_sign_zero( v - base );
    return v;
  }

private:
  void write_byte(uint8_t b)
  {
    buf[pos++] = b;
    if( pos == kBufCap )
    {
      sink.write(buf.data(), pos);
      total_sz+=pos;
      pos = 0;
    }
  }
};

struct BitReader
{
  const uint8_t* p;
  const uint8_t* end;
  uint64_t acc = 0;
  unsigned bits = 0;

  BitReader(const uint8_t* p_, const uint8_t* end_);

  uint64_t get(unsigned b)
  {
    if( !b )
      return 0;
    while( bits < b )
    {
      if( p == end )
        throw std::runtime_error("bitstream underflow");
      acc |= uint64_t(*p++) << bits;
      bits += 8;
    }
    const uint64_t mask = (b == 64) ? ~0ull : ((1ull << b) - 1);
    uint64_t v = acc & mask;
    acc >>= b;
    bits -= b;
    return v;
  }

  uint64_t get_var64()
  {
    uint64_t v = 0;
    unsigned shift = 0;
    for( ;; )
    {
      uint8_t b = get(8);
      v |= uint64_t(b & 0x7F) << shift;
      if( (b & 0x80) == 0 )
        return v;
      shift += 7;
      if( shift >= 64 )
        throw std::runtime_error("bad varint");
    }
  }
  uint64_t get_var64_zero()
  {
    if( get(1) )
      return 0;

    return get_var64();
  }
  uint64_t get_var64_dec_zeros()
  {
    if( get(1) )
      return 0;

    unsigned k = (unsigned)get(4);
    uint64_t v = get_var64();
    return v * POW10[k];
  }
  int64_t get_var64_sign_dec_zeros()
  {
    if( get(1) )
      return 0;

    unsigned k = (unsigned)get(4);
    return zigzag_decode( get_var64() ) * POW10[k];
  }
  uint64_t get_var64_sign_zero()
  {
    if( get(1) )
      return 0;

    return zigzag_decode(get_var64());
  }
};

}

