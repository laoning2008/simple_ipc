#pragma once
#include "named_synchronization_object.hpp"

class named_event_t : public named_synchronization_object_t {
public:
    named_event_t(const std::string& name, bool newly) {
        if (newly) {
            create(name, 1);
        } else {
            open(name);
        }
    }

    ~named_event_t() {
        destruct();
    }

    void set() {
        post_impl();
    }

    void wait() {
        wait_impl();
    }
};