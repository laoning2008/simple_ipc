#pragma once

#include <cstdint>
#include <stdexcept>
#include <unistd.h>
#include <cassert>
#include <memory>
#include <iostream>

namespace simple::ipc {

        class linear_ringbuffer_t {
        public:
            explicit linear_ringbuffer_t(size_t capacity = 0) noexcept
                    : capacity_(capacity), head_(0), tail_(0), size_(0) {}

            void commit(size_t n) noexcept {
                assert(n <= (capacity_ - size_));
                tail_ = (tail_ + n) % capacity_;
                size_ += n;
            }

            void consume(size_t n) noexcept {
                assert(n <= size_);
                head_ = (head_ + n) % capacity_;
                size_ -= n;
            }

            uint8_t* read_head(unsigned char *buffer) noexcept {
                return buffer + head_;
            }

            uint8_t* write_head(unsigned char *buffer) noexcept {
                return buffer + tail_;
            }

            void clear() noexcept {
                tail_ = head_ = size_ = 0;
            }


            [[nodiscard]] size_t size() const noexcept {
                return size_;
            }

            [[nodiscard]] bool empty() const noexcept {
                return size_ == 0;
            }

            [[nodiscard]] size_t capacity() const noexcept {
                return capacity_;
            }

            [[nodiscard]] size_t free_size() const noexcept {
                return capacity_ - size_;
            }
        private:
            size_t capacity_{};
            size_t head_{};
            size_t tail_{};
            size_t size_{};
        };

}