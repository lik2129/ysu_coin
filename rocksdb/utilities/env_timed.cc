// Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "monitoring/perf_context_imp.h"
#include "rocksdb/env.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

#ifndef ROCKSDB_LITE

// An environment that measures function call times for filesystem
// operations, reporting results to variables in PerfContext.
class TimedEnv : public EnvWrapper {
 public:
  explicit TimedEnv(Env* base_env) : EnvWrapper(base_env) {}

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result,
                           const EnvOptions& options) override {
    PERF_TIMER_GUARD(env_new_sequential_file_ysus);
    return EnvWrapper::NewSequentialFile(fname, result, options);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result,
                             const EnvOptions& options) override {
    PERF_TIMER_GUARD(env_new_random_access_file_ysus);
    return EnvWrapper::NewRandomAccessFile(fname, result, options);
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result,
                         const EnvOptions& options) override {
    PERF_TIMER_GUARD(env_new_writable_file_ysus);
    return EnvWrapper::NewWritableFile(fname, result, options);
  }

  Status ReuseWritableFile(const std::string& fname,
                           const std::string& old_fname,
                           std::unique_ptr<WritableFile>* result,
                           const EnvOptions& options) override {
    PERF_TIMER_GUARD(env_reuse_writable_file_ysus);
    return EnvWrapper::ReuseWritableFile(fname, old_fname, result, options);
  }

  Status NewRandomRWFile(const std::string& fname,
                         std::unique_ptr<RandomRWFile>* result,
                         const EnvOptions& options) override {
    PERF_TIMER_GUARD(env_new_random_rw_file_ysus);
    return EnvWrapper::NewRandomRWFile(fname, result, options);
  }

  Status NewDirectory(const std::string& name,
                      std::unique_ptr<Directory>* result) override {
    PERF_TIMER_GUARD(env_new_directory_ysus);
    return EnvWrapper::NewDirectory(name, result);
  }

  Status FileExists(const std::string& fname) override {
    PERF_TIMER_GUARD(env_file_exists_ysus);
    return EnvWrapper::FileExists(fname);
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    PERF_TIMER_GUARD(env_get_children_ysus);
    return EnvWrapper::GetChildren(dir, result);
  }

  Status GetChildrenFileAttributes(
      const std::string& dir, std::vector<FileAttributes>* result) override {
    PERF_TIMER_GUARD(env_get_children_file_attributes_ysus);
    return EnvWrapper::GetChildrenFileAttributes(dir, result);
  }

  Status DeleteFile(const std::string& fname) override {
    PERF_TIMER_GUARD(env_delete_file_ysus);
    return EnvWrapper::DeleteFile(fname);
  }

  Status CreateDir(const std::string& dirname) override {
    PERF_TIMER_GUARD(env_create_dir_ysus);
    return EnvWrapper::CreateDir(dirname);
  }

  Status CreateDirIfMissing(const std::string& dirname) override {
    PERF_TIMER_GUARD(env_create_dir_if_missing_ysus);
    return EnvWrapper::CreateDirIfMissing(dirname);
  }

  Status DeleteDir(const std::string& dirname) override {
    PERF_TIMER_GUARD(env_delete_dir_ysus);
    return EnvWrapper::DeleteDir(dirname);
  }

  Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    PERF_TIMER_GUARD(env_get_file_size_ysus);
    return EnvWrapper::GetFileSize(fname, file_size);
  }

  Status GetFileModificationTime(const std::string& fname,
                                 uint64_t* file_mtime) override {
    PERF_TIMER_GUARD(env_get_file_modification_time_ysus);
    return EnvWrapper::GetFileModificationTime(fname, file_mtime);
  }

  Status RenameFile(const std::string& src, const std::string& dst) override {
    PERF_TIMER_GUARD(env_rename_file_ysus);
    return EnvWrapper::RenameFile(src, dst);
  }

  Status LinkFile(const std::string& src, const std::string& dst) override {
    PERF_TIMER_GUARD(env_link_file_ysus);
    return EnvWrapper::LinkFile(src, dst);
  }

  Status LockFile(const std::string& fname, FileLock** lock) override {
    PERF_TIMER_GUARD(env_lock_file_ysus);
    return EnvWrapper::LockFile(fname, lock);
  }

  Status UnlockFile(FileLock* lock) override {
    PERF_TIMER_GUARD(env_unlock_file_ysus);
    return EnvWrapper::UnlockFile(lock);
  }

  Status NewLogger(const std::string& fname,
                   std::shared_ptr<Logger>* result) override {
    PERF_TIMER_GUARD(env_new_logger_ysus);
    return EnvWrapper::NewLogger(fname, result);
  }
};

Env* NewTimedEnv(Env* base_env) { return new TimedEnv(base_env); }

#else  // ROCKSDB_LITE

Env* NewTimedEnv(Env* /*base_env*/) { return nullptr; }

#endif  // !ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE
