// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <sstream>
#include "monitoring/iostats_context_imp.h"
#include "rocksdb/env.h"

namespace ROCKSDB_NAMESPACE {

#ifdef ROCKSDB_SUPPORT_THREAD_LOCAL
__thread IOStatsContext iostats_context;
#endif

IOStatsContext* get_iostats_context() {
#ifdef ROCKSDB_SUPPORT_THREAD_LOCAL
  return &iostats_context;
#else
  return nullptr;
#endif
}

void IOStatsContext::Reset() {
  thread_pool_id = Env::Priority::TOTAL;
  bytes_read = 0;
  bytes_written = 0;
  open_ysus = 0;
  allocate_ysus = 0;
  write_ysus = 0;
  read_ysus = 0;
  range_sync_ysus = 0;
  prepare_write_ysus = 0;
  fsync_ysus = 0;
  logger_ysus = 0;
}

#define IOSTATS_CONTEXT_OUTPUT(counter)         \
  if (!exclude_zero_counters || counter > 0) {  \
    ss << #counter << " = " << counter << ", "; \
  }

std::string IOStatsContext::ToString(bool exclude_zero_counters) const {
  std::ostringstream ss;
  IOSTATS_CONTEXT_OUTPUT(thread_pool_id);
  IOSTATS_CONTEXT_OUTPUT(bytes_read);
  IOSTATS_CONTEXT_OUTPUT(bytes_written);
  IOSTATS_CONTEXT_OUTPUT(open_ysus);
  IOSTATS_CONTEXT_OUTPUT(allocate_ysus);
  IOSTATS_CONTEXT_OUTPUT(write_ysus);
  IOSTATS_CONTEXT_OUTPUT(read_ysus);
  IOSTATS_CONTEXT_OUTPUT(range_sync_ysus);
  IOSTATS_CONTEXT_OUTPUT(fsync_ysus);
  IOSTATS_CONTEXT_OUTPUT(prepare_write_ysus);
  IOSTATS_CONTEXT_OUTPUT(logger_ysus);

  std::string str = ss.str();
  str.erase(str.find_last_not_of(", ") + 1);
  return str;
}

}  // namespace ROCKSDB_NAMESPACE
