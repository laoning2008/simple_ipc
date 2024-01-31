#pragma once
#include "named_synchronization_object.hpp"

class named_mutex_t : public named_synchronization_object_t {
public:
    named_mutex_t(const std::string& name, bool newly) {
        if (newly) {
            create(name, 1);
        } else {
            open(name);
        }
    }

    ~named_mutex_t() {
        destruct();
    }

    void unlock() {
        post_impl();
    }

    void lock() {
        wait_impl();
    }
};