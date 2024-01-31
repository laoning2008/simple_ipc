#pragma once

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>

//https://github.com/a-darwish/memfd-examples/blob/master/client.c

class shared_mem_buf_t {
    struct control_block_t {
        pthread_mutex_t c_lock;
        pthread_mutex_t s_lock;

        pthread_cond_t c_can_r_con;
        pthread_cond_t c_can_w_con;
        pthread_cond_t s_can_r_con;
        pthread_cond_t s_can_w_con;

        size_t rw_buf_len;
        uint32_t c_r_pos;
        uint32_t c_w_pos;
        uint32_t s_r_pos;
        uint32_t s_w_pos;
    };
public:
    class construct_failed_exception : public std::exception {};
    class invalid_state_exception : public std::exception {};
public:
    shared_mem_buf_t(const std::string& name, bool create, size_t rw_buf_len) {
        auto shared_mem = map_shared_memory(name, create, rw_buf_len);
        if (!init_control_block(create, shared_mem, rw_buf_len)) {
            throw construct_failed_exception{};
        }
    }

    ~shared_mem_buf_t() {

    }

private:
    void* map_shared_memory(const std::string& name, bool create, size_t rw_buf_len) {
        auto fd = memfd_create(name.c_str(), MFD_CLOEXEC);
        if (fd == -1) {
            return nullptr;
        }

        auto size = cal_mem_size(rw_buf_len);
        if (create) {
            if (ftruncate(fd, size) == -1) {
                return nullptr;
            }
        }

        void* shared_mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED,fd, 0);
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
            if (create_synchronization_objects(control_block)) {
                return false;
            }

            control_block->rw_buf_len = rw_buf_len;
            control_block->c_r_pos = 0;
            control_block->c_w_pos = 0;
            control_block->s_r_pos = 0;
            control_block->s_w_pos = 0;
        }

        c_buf = (uint8_t*)control_block + sizeof(control_block_t);
        s_buf = (uint8_t*)c_buf + rw_buf_len;
        return true;
    }

    bool create_synchronization_objects(control_block_t* con_block) {
        do {
            pthread_mutexattr_t mutex_attr;
            if (pthread_mutexattr_init(&mutex_attr) != 0) {
                break;
            }

            if (pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0) {
                break;
            }

            if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
                break;
            }

            if (pthread_mutex_init(&con_block->c_lock, &mutex_attr) != 0) {
                break;
            }

            if (pthread_mutex_init(&con_block->s_lock, &mutex_attr) != 0) {
                break;
            }

            pthread_condattr_t cond_attr;
            if (pthread_condattr_init(&cond_attr) != 0) {
                break;
            }
            if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
                break;
            }

            if (pthread_cond_init(&con_block->c_can_r_con, &cond_attr)  != 0) {
                break;
            }

            if (pthread_cond_init(&con_block->c_can_w_con, &cond_attr) != 0) {
                break;
            }

            if (pthread_cond_init(&con_block->s_can_r_con, &cond_attr) != 0) {
                break;
            }

            if (pthread_cond_init(&con_block->s_can_r_con, &cond_attr) != 0) {
                break;
            }

            return true;
        } while (0);

        return false;
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
    uint8_t * c_buf;
    uint8_t* s_buf;
    control_block_t* control_block;
};