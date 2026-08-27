// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A portable implementation of crc32c.

#include "util/crc32c.h"

#include <stddef.h>
#include <stdint.h>

#include "port/port.h"
#include "util/coding.h"

namespace leveldb {
namespace crc32c {

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
    return port::AcceleratedCRC32C(crc, data, n);
}

}  // namespace crc32c
}  // namespace leveldb
