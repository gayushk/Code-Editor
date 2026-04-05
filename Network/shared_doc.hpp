#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <iostream>

constexpr const char* SHM_NAME = "/collab_editor_doc";
constexpr size_t      DOC_CAP  = 1 << 20;

struct SharedDoc {
    pthread_rwlock_t rwlock;
    size_t           length;
    char             buf[DOC_CAP];

    void set(const std::string& s) {
        if (s.size() >= DOC_CAP) {
            std::cerr << "shared_doc: document truncated to " << DOC_CAP << " bytes\n";
        }
        length = s.size() < DOC_CAP ? s.size() : DOC_CAP - 1;
        std::memcpy(buf, s.data(), length);
    }
    std::string get() const {
        return std::string(buf, length);
    }
};

struct RdLock {
    pthread_rwlock_t& lk;
    explicit RdLock(pthread_rwlock_t& l) : lk(l) { pthread_rwlock_rdlock(&lk); }
    ~RdLock() { pthread_rwlock_unlock(&lk); }
};

struct WrLock {
    pthread_rwlock_t& lk;
    explicit WrLock(pthread_rwlock_t& l) : lk(l) { pthread_rwlock_wrlock(&lk); }
    ~WrLock() { pthread_rwlock_unlock(&lk); }
};

inline SharedDoc* shm_create() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("shm_open(create)");
    if (ftruncate(fd, sizeof(SharedDoc)) < 0) {
        close(fd);
        throw std::runtime_error("ftruncate");
    }
    auto* doc = static_cast<SharedDoc*>(
        mmap(nullptr, sizeof(SharedDoc), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    if (doc == MAP_FAILED) throw std::runtime_error("mmap");

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&doc->rwlock, &attr);
    pthread_rwlockattr_destroy(&attr);
    doc->length = 0;
    return doc;
}

inline SharedDoc* shm_open_doc() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd < 0) throw std::runtime_error("shm_open(open)");
    auto* doc = static_cast<SharedDoc*>(
        mmap(nullptr, sizeof(SharedDoc), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    if (doc == MAP_FAILED) throw std::runtime_error("mmap");
    return doc;
}

inline void shm_close(SharedDoc* doc) {
    munmap(doc, sizeof(SharedDoc));
}

inline void shm_destroy(SharedDoc* doc) {
    pthread_rwlock_destroy(&doc->rwlock);
    munmap(doc, sizeof(SharedDoc));
    shm_unlink(SHM_NAME);
}

