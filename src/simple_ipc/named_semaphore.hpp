#pragma once
#include "named_synchronization_object.hpp"

class named_semaphore_t : public named_synchronization_object_t {
public:
    named_semaphore_t(const std::string& name) {
        open(name);
    }

    named_semaphore_t(const std::string& name, uint32_t value) {
        create(name, value);
    }

    ~named_semaphore_t() {
        destruct();
    }

    void post() {
        post_impl();
    }

    void wait() {
        wait_impl();
    }
};