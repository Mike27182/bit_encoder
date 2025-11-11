/*
* codec.cpp
* Author: Mikhail Gorbunov, RIT
* Copyright(c) 2025. All rights reserved.
*/

#include "codec.h"
#include <zstd.h>
#include <stdexcept>
#include <cstring>
#include "common/types.h"

namespace RIT::MD
{

OStreamSink::OStreamSink(std::ostream& s)
:
  os{ s }
{
}

void OStreamSink::write(const uint8_t* data, size_t n)
{
  os.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
}

void OStreamSink::flush()
{
  os.flush();
}

void OStreamSink::finish()
{
  os.flush();
}

VectorSink::VectorSink(std::vector<uint8_t>& v)
:
  out{ v }
{
}

void VectorSink::write(const uint8_t* data, size_t n)
{
  out.insert(out.end(), data, data + n);
}

void VectorSink::flush()
{
}
void VectorSink::finish()
{
}

RawBufferSink::RawBufferSink(void* p, size_t c)
:
  dst{ static_cast<uint8_t*>(p) },
  cap{ c },
  pos{ 0 }
{
}

void RawBufferSink::write(const uint8_t* data, size_t n)
{
  if( pos + n > cap )
    throw std::runtime_error("RawBufferSink overflow");
  std::memcpy(dst + pos, data, n);
  pos += n;
}

void RawBufferSink::flush()
{
}
void RawBufferSink::finish()
{
}

ZstdStreamCompressor::ZstdStreamCompressor(ISink& downstream, int lvl)
:
  down{ downstream },
  cctx{ nullptr },
  level{ lvl },
  out_buf{}
{
  cctx = ZSTD_createCCtx();
  if( !cctx )
    throw std::runtime_error("ZSTD_createCCtx failed");
  size_t rc = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
  if( ZSTD_isError(rc) )
    throw std::runtime_error(ZSTD_getErrorName(rc));
}

ZstdStreamCompressor::~ZstdStreamCompressor()
{
  if( cctx )
    ZSTD_freeCCtx(cctx);
}

void ZstdStreamCompressor::write(const uint8_t* data, size_t n)
{
  ZSTD_inBuffer inb{ data, n, 0 };
  for( ;; )
  {
    ZSTD_outBuffer outb{ out_buf.data(), out_buf.size(), 0 };
    size_t rc = ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_continue);
    if( ZSTD_isError(rc) )
      throw std::runtime_error(ZSTD_getErrorName(rc));
    if( outb.pos )
      down.write(out_buf.data(), outb.pos);
    if( inb.pos == inb.size && outb.pos < outb.size )
      break;
  }
}

void ZstdStreamCompressor::flush()
{
  ZSTD_inBuffer inb{ nullptr, 0, 0 };
  for( ;; )
  {
    ZSTD_outBuffer outb{ out_buf.data(), out_buf.size(), 0 };
    size_t rc = ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_flush);
    if( ZSTD_isError(rc) )
      throw std::runtime_error(ZSTD_getErrorName(rc));
    if( outb.pos )
      down.write(out_buf.data(), outb.pos);
    if( outb.pos < outb.size )
      break;
  }
  down.flush();
}

void ZstdStreamCompressor::finish()
{
  ZSTD_inBuffer inb{ nullptr, 0, 0 };
  for( ;; )
  {
    ZSTD_outBuffer outb{ out_buf.data(), out_buf.size(), 0 };
    size_t rc = ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_end);
    if( ZSTD_isError(rc) )
      throw std::runtime_error(ZSTD_getErrorName(rc));
    if( outb.pos )
      down.write(out_buf.data(), outb.pos);
    if( rc == 0 )
      break;
  }
  down.finish();
}

BufferedBitWriter::BufferedBitWriter(ISink& s)
:
  sink{ s }
{
}

BitReader::BitReader(const uint8_t* p_, const uint8_t* end_)
:
  p{ p_ }, end{ end_ }
{
}

static int reg3 = add_test( []()
{
} );

}

