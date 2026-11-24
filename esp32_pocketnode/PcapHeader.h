#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pocketnode {

constexpr size_t kPcapGlobalHeaderSize = 24;

void makePcapGlobalHeader(uint32_t snapshot_length,
                          uint8_t output[kPcapGlobalHeaderSize]);

}  // namespace pocketnode
