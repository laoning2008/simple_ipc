#pragma once
#include <fcntl.h>           /* For O_* constants */
#include <sys/stat.h>        /* For mode constants */
#include <semaphore.h>
#include <time.h>
#include <sys/sem.h>

#include <string>
#include <exception>
#include <stdint.h>


class named_synchronization_object_t {
public:
    class construct_failed_exception : public std::exception {};
    class invalid_state_exception : public std::exception {};
protected:
    void open(const std::string& name) {
        sem = sem_open(name.c_str(), O_RDWR);

        if (sem == SEM_FAILED) {
            throw construct_failed_exception{};
        }
    }

    void create(const std::string& name, uint32_t value) {
        sem = sem_open(name.c_str(), O_CREAT|O_EXCL, DEFFILEMODE, value);

        if (sem == SEM_FAILED) {
            throw construct_failed_exception{};
        }
    }

    void destruct() {
        sem_close(sem);
    }

    void post_impl() {
        int ret = sem_post(sem);
        if (ret != 0) {
            throw invalid_state_exception{};
        }
    }

    void wait_impl() {
        int ret = -1;
        while ((ret = sem_wait(sem)) == -1 && errno == EINTR) {
            continue;
        }

        if (ret != 0) {
            throw invalid_state_exception{};
        }
    }
private:
    sem_t* sem;
};