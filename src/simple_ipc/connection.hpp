#pragma once

#include <pthread.h>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include "ringbuffer.hpp"

//https://github.com/a-darwish/memfd-examples/blob/master/client.c

class connection_t {
    struct control_block_t {
        pthread_mutex_t c_lock;
        pthread_mutex_t s_lock;

        pthread_cond_t c_can_r_con;
        pthread_cond_t c_can_w_con;
        pthread_cond_t s_can_r_con;
        pthread_cond_t s_can_w_con;

        linear_ringbuffer_t c_buf;
        linear_ringbuffer_t s_buf;
    };
public:
    class construct_failed_exception : public std::exception {};
    class invalid_state_exception : public std::exception {};
public:
    connection_t(int mem_fd, size_t rw_buf_len, bool create) : fd(mem_fd)
        , control_block(nullptr), mm_len(cal_mem_size(rw_buf_len)) {
        auto shared_mem = map_shared_memory(rw_buf_len, create);
        if (!init_control_block(create, shared_mem, rw_buf_len)) {
            throw construct_failed_exception{};
        }
    }

    ~connection_t() {
        destroy_synchronization_objects();
        close(fd);
        munmap(control_block, mm_len);
    }

    void send_packet(void* buf, size_t buf_len) {
        pthread_mutex_lock(&control_block->c_lock);

    }
private:

    void* map_shared_memory(size_t rw_buf_len, bool create) {
        if (create) {
            if (ftruncate(fd, mm_len) == -1) {
                return nullptr;
            }
        }

        void* shared_mem = mmap(nullptr, mm_len, PROT_READ|PROT_WRITE, MAP_SHARED,fd, 0);
        if(shared_mem == MAP_FAILED) {
            return nullptr;
        }

        return shared_mem;
    }

    bool init_control_block(bool create, void* shared_mem, size_t rw_buf_len) {
        if (shared_mem == nullptr) {
            return false;
        }

        control_block = static_cast<control_block_t*>(shared_mem);

        if (create) {
            if (create_synchronization_objects()) {
                return false;
            }

            control_block->c_buf = linear_ringbuffer_t{(uint8_t*)control_block + sizeof(control_block_t), rw_buf_len};
            control_block->s_buf = linear_ringbuffer_t{(uint8_t*)control_block + sizeof(control_block_t) + rw_buf_len, rw_buf_len};
        }

        return true;
    }

    bool create_synchronization_objects() {
        pthread_mutexattr_t mutex_attr;
        if (pthread_mutexattr_init(&mutex_attr) != 0) {
            return false;
        }

        if (pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0) {
            return false;
        }

        if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
            return false;
        }

        if (pthread_mutex_init(&control_block->c_lock, &mutex_attr) != 0) {
            return false;
        }

        if (pthread_mutex_init(&control_block->s_lock, &mutex_attr) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            return false;
        }

        pthread_condattr_t cond_attr;
        if (pthread_condattr_init(&cond_attr) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            return false;
        }
        if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            return false;
        }

        if (pthread_cond_init(&control_block->c_can_r_con, &cond_attr)  != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            return false;
        }

        if (pthread_cond_init(&control_block->c_can_w_con, &cond_attr) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            pthread_cond_destroy(&control_block->c_can_r_con);
            return false;
        }

        if (pthread_cond_init(&control_block->s_can_r_con, &cond_attr) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            pthread_cond_destroy(&control_block->c_can_r_con);
            pthread_cond_destroy(&control_block->c_can_w_con);
            return false;
        }

        if (pthread_cond_init(&control_block->s_can_w_con, &cond_attr) != 0) {
            pthread_mutex_destroy(&control_block->c_lock);
            pthread_mutex_destroy(&control_block->s_lock);
            pthread_cond_destroy(&control_block->c_can_r_con);
            pthread_cond_destroy(&control_block->c_can_w_con);
            pthread_cond_destroy(&control_block->s_can_r_con);
            return false;
        }

        return true;
    }

    void destroy_synchronization_objects() {
        pthread_mutex_destroy(&control_block->c_lock);
        pthread_mutex_destroy(&control_block->s_lock);
        pthread_cond_destroy(&control_block->c_can_r_con);
        pthread_cond_destroy(&control_block->c_can_w_con);
        pthread_cond_destroy(&control_block->s_can_r_con);
        pthread_cond_destroy(&control_block->s_can_w_con);
    }

    size_t cal_mem_size(size_t rw_buf_len) {
        static const size_t PAGE_SIZE = sysconf(_SC_PAGESIZE);
        size_t min_size = sizeof(control_block_t) + 2 * rw_buf_len;

        // Round up to nearest multiple of page size.
        size_t actual_bytes = min_size & ~(PAGE_SIZE - 1);
        if (min_size % PAGE_SIZE) {
            actual_bytes += PAGE_SIZE;
        }

        return actual_bytes;
    }

private:
    int fd;
    control_block_t* control_block;
    size_t mm_len;
};