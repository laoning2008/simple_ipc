#pragma once

#include <assert.h>
#include <cstdint>
#include <stdexcept>
#include <unistd.h>

namespace simple { namespace ipc {

        class linear_ringbuffer_t {
        public:
            typedef unsigned char value_type;
            typedef value_type &reference;
            typedef const value_type &const_reference;
            typedef value_type *iterator;
            typedef const value_type *const_iterator;

            linear_ringbuffer_t(unsigned char *buf = nullptr, size_t capacity = 0) noexcept
                    : buffer_(buf), capacity_(capacity), head_(0), tail_(0), size_(0) {}

            ~linear_ringbuffer_t() {
            }

            linear_ringbuffer_t(linear_ringbuffer_t &&other) noexcept {
                linear_ringbuffer_t tmp{};
                tmp.swap(other);
                this->swap(tmp);
            }

            linear_ringbuffer_t &operator=(linear_ringbuffer_t &&other) noexcept {
                linear_ringbuffer_t tmp{};
                tmp.swap(other);
                this->swap(tmp);
                return *this;
            }

            linear_ringbuffer_t(const linear_ringbuffer_t &) = delete;
            linear_ringbuffer_t &operator=(const linear_ringbuffer_t &) = delete;

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

            iterator read_head() noexcept {
                return buffer_ + head_;
            }

            iterator write_head() noexcept {
                return buffer_ + tail_;
            }

            void clear() noexcept {
                tail_ = head_ = size_ = 0;
            }


            size_t size() const noexcept {
                return size_;
            }

            bool empty() const noexcept {
                return size_ == 0;
            }

            size_t capacity() const noexcept {
                return capacity_;
            }

            size_t free_size() const noexcept {
                return capacity_ - size_;
            }

            const_iterator cbegin() const noexcept {
                return buffer_ + head_;
            }

            const_iterator begin() const noexcept {
                return cbegin();
            }

            const_iterator cend() const noexcept {
                // Fix up `end` if needed so that [begin, end) is always a
                // valid range.
                return head_ < tail_ ?
                       buffer_ + tail_ :
                       buffer_ + tail_ + capacity_;
            }

            const_iterator end() const noexcept {
                return cend();
            }

            void swap(linear_ringbuffer_t &other) noexcept {
                using std::swap;
                swap(buffer_, other.buffer_);
                swap(capacity_, other.capacity_);
                swap(tail_, other.tail_);
                swap(head_, other.head_);
                swap(size_, other.size_);
            }
        private:
            unsigned char *buffer_;
            size_t capacity_;
            size_t head_;
            size_t tail_;
            size_t size_;
        };


        void swap(linear_ringbuffer_t &lhs, linear_ringbuffer_t &rhs) noexcept {
            lhs.swap(rhs);
        }

}}