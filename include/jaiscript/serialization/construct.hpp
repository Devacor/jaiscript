#pragma once
#ifndef JAI_SERIALIZATION_CONSTRUCT_HPP
#define JAI_SERIALIZATION_CONSTRUCT_HPP

#include <memory>
#include <jaiscript/serialization/archive_impl.hpp>

namespace jai {

namespace serialization {
	template<typename T> class construct;
	template<typename T> class construct_unique;
}

// jai::access lives in traits.hpp (it must be complete before archive_impl.hpp's
// trait specializations name it; this header includes archive_impl.hpp first).

namespace serialization {

	template<typename T>
	class construct {
		std::shared_ptr<T>& ptr_ref_;
		bool constructed_ = false;
		std::function<void()> on_construct_;
	public:
		explicit construct(std::shared_ptr<T>& ptr) : ptr_ref_(ptr) {}

		// Fires eagerly inside operator() right after the shared_ptr exists, so child weak_ptrs
		// can resolve back to this object during load_and_construct.
		void set_on_construct(std::function<void()> cb) { on_construct_ = std::move(cb); }

		template<typename... Args>
		void operator()(Args&&... args) {
			ptr_ref_ = ::jai::access::make_shared<T>(std::forward<Args>(args)...);
			constructed_ = true;
			if (on_construct_) on_construct_();
		}

		T* operator->() { return ptr_ref_.get(); }
		T& operator*() { return *ptr_ref_; }
		bool is_constructed() const { return constructed_; }
		std::shared_ptr<T>& get_ptr() { return ptr_ref_; }
	};

	template<typename T>
	class construct_unique {
		std::unique_ptr<T>& ptr_ref_;
		bool constructed_ = false;
	public:
		explicit construct_unique(std::unique_ptr<T>& ptr) : ptr_ref_(ptr) {}

		template<typename... Args>
		void operator()(Args&&... args) {
			ptr_ref_ = ::jai::access::make_unique<T>(std::forward<Args>(args)...);
			constructed_ = true;
		}

		T* operator->() { return ptr_ref_.get(); }
		T& operator*() { return *ptr_ref_; }
		bool is_constructed() const { return constructed_; }
		std::unique_ptr<T>& get_ptr() { return ptr_ref_; }
	};

} // namespace serialization
} // namespace jai

#endif // JAI_SERIALIZATION_CONSTRUCT_HPP
