#pragma once

#ifndef __JAISCRIPT_CORE_BOUND_CPP_VECTOR_HPP__
#define __JAISCRIPT_CORE_BOUND_CPP_VECTOR_HPP__

#include "value.hpp"
#include "types.hpp"
#include <memory>
#include <vector>
#include <iterator>
#include <algorithm>
#include <stdexcept>

namespace jai {

// Forward declarations
class engine;

/**
 * @brief Zero-copy wrapper for C++ std::vector<T> with lazy conversion to script_value
 *
 * This wrapper provides script access to native C++ vectors without eagerly copying all elements.
 * Elements are converted to script_value on-demand during access.
 *
 * Lifetime assumption: The wrapped vector must remain valid during the wrapper's use.
 * The interpreter keeps parent objects on the stack during expression evaluation, ensuring this.
 *
 * @tparam T The element type of the C++ vector (e.g., Cat, Mouse)
 */
template<typename T>
class bound_cpp_vector {
private:
    std::vector<T>* vec_;  // Raw pointer to C++ vector (assumes owner stays alive)
    engine* engine_ref_;

public:
    // Type aliases for STL compatibility
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;

    /**
     * @brief Construct from C++ vector reference
     * @param vec Reference to existing C++ std::vector<T>
     * @param eng Engine pointer for creating script_values
     */
    explicit bound_cpp_vector(std::vector<T>& vec, engine* eng)
        : vec_(&vec), engine_ref_(eng) {}

    /**
     * @brief Copy constructor (creates another wrapper to the same vector)
     */
    bound_cpp_vector(const bound_cpp_vector& other)
        : vec_(other.vec_), engine_ref_(other.engine_ref_) {}

    /**
     * @brief Move constructor
     */
    bound_cpp_vector(bound_cpp_vector&& other) noexcept
        : vec_(other.vec_), engine_ref_(std::move(other.engine_ref_)) {
        other.vec_ = nullptr;
    }

    /**
     * @brief Copy assignment
     */
    bound_cpp_vector& operator=(const bound_cpp_vector& other) {
        if (this != &other) {
            vec_ = other.vec_;
            engine_ref_ = other.engine_ref_;
        }
        return *this;
    }

    /**
     * @brief Move assignment
     */
    bound_cpp_vector& operator=(bound_cpp_vector&& other) noexcept {
        if (this != &other) {
            vec_ = other.vec_;
            engine_ref_ = std::move(other.engine_ref_);
            other.vec_ = nullptr;
        }
        return *this;
    }

    // ===== Capacity =====

    size_type size() const { return vec_ ? vec_->size() : 0; }
    bool empty() const { return !vec_ || vec_->empty(); }

    void reserve(size_type new_cap) {
        if (vec_) vec_->reserve(new_cap);
    }

    size_type capacity() const { return vec_ ? vec_->capacity() : 0; }

    // ===== Modifiers =====

    void clear() {
        if (vec_) vec_->clear();
    }

    void push_back(const T& value) {
        if (!vec_) throw runtime_error("Cannot push_back to null bound_cpp_vector");
        vec_->push_back(value);
    }

    void push_back(T&& value) {
        if (!vec_) throw runtime_error("Cannot push_back to null bound_cpp_vector");
        vec_->push_back(std::move(value));
    }

    void pop_back() {
        if (vec_ && !vec_->empty()) {
            vec_->pop_back();
        }
    }

    void resize(size_type count) {
        if (!vec_) throw runtime_error("Cannot resize null bound_cpp_vector");
        vec_->resize(count);
    }

    void resize(size_type count, const T& value) {
        if (!vec_) throw runtime_error("Cannot resize null bound_cpp_vector");
        vec_->resize(count, value);
    }

    // ===== Element access =====

    // For script access: returns T& which will be converted to script_value by interpreter
    T& operator[](size_type pos) {
        if (!vec_ || pos >= vec_->size()) {
            throw std::out_of_range("bound_cpp_vector::operator[]: index out of range");
        }
        return (*vec_)[pos];
    }

    const T& operator[](size_type pos) const {
        if (!vec_ || pos >= vec_->size()) {
            throw std::out_of_range("bound_cpp_vector::operator[]: index out of range");
        }
        return (*vec_)[pos];
    }

    T& at(size_type pos) {
        if (!vec_ || pos >= vec_->size()) {
            throw std::out_of_range("bound_cpp_vector::at: index out of range");
        }
        return (*vec_)[pos];
    }

    const T& at(size_type pos) const {
        if (!vec_ || pos >= vec_->size()) {
            throw std::out_of_range("bound_cpp_vector::at: index out of range");
        }
        return (*vec_)[pos];
    }

    T& front() {
        if (!vec_ || vec_->empty()) {
            throw runtime_error("bound_cpp_vector::front: vector is empty");
        }
        return vec_->front();
    }

    const T& front() const {
        if (!vec_ || vec_->empty()) {
            throw runtime_error("bound_cpp_vector::front: vector is empty");
        }
        return vec_->front();
    }

    T& back() {
        if (!vec_ || vec_->empty()) {
            throw runtime_error("bound_cpp_vector::back: vector is empty");
        }
        return vec_->back();
    }

    const T& back() const {
        if (!vec_ || vec_->empty()) {
            throw runtime_error("bound_cpp_vector::back: vector is empty");
        }
        return vec_->back();
    }

    // ===== Direct vector access =====

    /**
     * @brief Get underlying C++ vector pointer
     */
    std::vector<T>* get_vector() { return vec_; }
    const std::vector<T>* get_vector() const { return vec_; }

    // ===== Iterators =====

    // For C++ iteration over the wrapper
    typename std::vector<T>::iterator begin() { return vec_ ? vec_->begin() : typename std::vector<T>::iterator(); }
    typename std::vector<T>::iterator end() { return vec_ ? vec_->end() : typename std::vector<T>::iterator(); }

    typename std::vector<T>::const_iterator begin() const { return vec_ ? vec_->begin() : typename std::vector<T>::const_iterator(); }
    typename std::vector<T>::const_iterator end() const { return vec_ ? vec_->end() : typename std::vector<T>::const_iterator(); }

    typename std::vector<T>::const_iterator cbegin() const { return begin(); }
    typename std::vector<T>::const_iterator cend() const { return end(); }
};

} // namespace jai

#endif // __JAISCRIPT_CORE_BOUND_CPP_VECTOR_HPP__
