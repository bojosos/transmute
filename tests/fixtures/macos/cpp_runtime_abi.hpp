#pragma once
#include <condition_variable>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sys/stat.h>

// Compile-time checks for Alchemy's selected LP64 guest runtime profile.
// Stock Apple libc++ variants still require their own symbol/layout audits.
static_assert(sizeof(void*) == 8 && sizeof(long) == 8 && sizeof(long double) == 8);
static_assert(sizeof(wchar_t) == 4 && wchar_t(-1) < 0);
static_assert(sizeof(mbstate_t) == 128 && alignof(mbstate_t) == 8);
static_assert(sizeof(timespec) == 16 && offsetof(timespec, tv_nsec) == 8);
static_assert(sizeof(timeval) == 16 && offsetof(timeval, tv_usec) == 8);
static_assert(sizeof(pthread_t) == 8 && sizeof(pthread_key_t) == 8);
static_assert(sizeof(*pthread_t{}) == 8192 && offsetof(_opaque_pthread_t, cleanup) == 8);
static_assert(sizeof(alchemy_pthread_cleanup_record) == 24);
static_assert(sizeof(pthread_mutex_t) == 64 && alignof(pthread_mutex_t) == 8);
static_assert(sizeof(pthread_cond_t) == 48 && sizeof(pthread_rwlock_t) == 200);
static_assert(sizeof(pthread_once_t) == 16 && sizeof(pthread_attr_t) == 64);
static_assert(sizeof(struct stat) == 144 && alignof(struct stat) == 8);
static_assert(offsetof(struct stat, st_mode) == 4 && offsetof(struct stat, st_ino) == 8);
static_assert(offsetof(struct stat, st_uid) == 16 && offsetof(struct stat, st_rdev) == 24);
static_assert(offsetof(struct stat, st_atimespec) == 32 && offsetof(struct stat, st_birthtimespec) == 80);
static_assert(offsetof(struct stat, st_size) == 96 && offsetof(struct stat, st_blocks) == 104);
static_assert(offsetof(struct stat, st_blksize) == 112 && offsetof(struct stat, st_flags) == 116);
static_assert(offsetof(struct stat, st_qspare) == 128);
static_assert(sizeof(std::string) == 24 && sizeof(std::vector<int>) == 24);
static_assert(sizeof(std::mutex) == 64 && sizeof(std::condition_variable) == 48);
static_assert(sizeof(std::thread) == 8 && sizeof(std::exception_ptr) == 8);
