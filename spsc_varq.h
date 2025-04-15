#pragma once
#include "msg_header.h"
#include <atomic>
#include <cstdint>

namespace tcpshm {

template<uint32_t Bytes>
class SPSCVarQueue
{
public:
  static constexpr uint32_t BLK_CNT = Bytes / 64;
  static_assert(BLK_CNT && !(BLK_CNT & (BLK_CNT - 1)), "BLK_CNT must be a power of 2");

  MsgHeader* Alloc(uint16_t size) {
    size += sizeof(MsgHeader);
    uint32_t blk_sz = (size + sizeof(Block) - 1) / sizeof(Block);
    uint32_t padding_sz = BLK_CNT - (write_idx % BLK_CNT);
    bool rewind = blk_sz > padding_sz;
    // min_read_idx could be a negtive value which results in a large unsigned int
    uint32_t min_read_idx = write_idx + blk_sz + (rewind ? padding_sz : 0) - BLK_CNT;
    if (static_cast<int>(read_idx_cach - min_read_idx) < 0) {
      read_idx_cach = read_idx.load(std::memory_order_acquire);
      if (static_cast<int>(read_idx_cach - min_read_idx) < 0) { // no enough space
        return nullptr;
      }
    }
    if (rewind) {
      blk[write_idx % BLK_CNT].header.size = 0;
      std::atomic_thread_fence(std::memory_order_release);
      write_idx += padding_sz;
    }
    MsgHeader& header = blk[write_idx % BLK_CNT].header;
    header.size = size;
    return &header;
  }

  void Push() {
    std::atomic_thread_fence(std::memory_order_release);
    uint32_t blk_sz = (blk[write_idx % BLK_CNT].header.size + sizeof(Block) - 1) / sizeof(Block);
    write_idx += blk_sz;
    write_idx_atom.store(write_idx, std::memory_order_release);
  }

  MsgHeader* Front() {
    uint32_t curr_write_idx = write_idx_atom.load(std::memory_order_acquire);
    uint32_t curr_read_idx = read_idx.load(std::memory_order_relaxed);
    
    if(curr_read_idx == curr_write_idx) {
      return nullptr;
    }
    
    uint16_t size = blk[curr_read_idx % BLK_CNT].header.size;
    if(size == 0) { // rewind
      curr_read_idx += BLK_CNT - (curr_read_idx % BLK_CNT);
      read_idx.store(curr_read_idx, std::memory_order_relaxed);
      
      if(curr_read_idx == curr_write_idx) {
        return nullptr;
      }
    }
    
    return &blk[curr_read_idx % BLK_CNT].header;
  }

  void Pop() {
    uint32_t curr_read_idx = read_idx.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    
    uint32_t blk_sz = (blk[curr_read_idx % BLK_CNT].header.size + sizeof(Block) - 1) / sizeof(Block);
    read_idx.store(curr_read_idx + blk_sz, std::memory_order_release);
  }

private:
  struct Block // size of 64, same as cache line
  {
    alignas(64) MsgHeader header;
  } blk[BLK_CNT];

  alignas(128) uint32_t write_idx = 0;
  alignas(128) std::atomic<uint32_t> write_idx_atom{0};
  uint32_t read_idx_cach = 0; // used only by writing thread

  alignas(128) std::atomic<uint32_t> read_idx{0};
};
} // namespace tcpshm
