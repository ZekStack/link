#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

template <typename Signature, size_t StorageSize> class LinkCallback;

template <typename ReturnType, size_t StorageSize, typename... Args>
class LinkCallback<ReturnType(Args...), StorageSize> {
  public:
	LinkCallback() = default;

	~LinkCallback() {
		reset();
	}

	LinkCallback(const LinkCallback &other) {
		copyFrom(other);
	}

	LinkCallback &operator=(const LinkCallback &other) {
		if (this != &other) {
			reset();
			copyFrom(other);
		}
		return *this;
	}

	LinkCallback(LinkCallback &&other) noexcept {
		moveFrom(other);
	}

	LinkCallback &operator=(LinkCallback &&other) noexcept {
		if (this != &other) {
			reset();
			moveFrom(other);
		}
		return *this;
	}

	template <typename Callable> bool assign(Callable callable) {
		using CallableType = std::decay_t<Callable>;
		static_assert(
		    std::is_invocable_r_v<ReturnType, CallableType &, Args...>,
		    "Link callback signature does not match"
		);

		if constexpr (sizeof(CallableType) > StorageSize ||
		              alignof(CallableType) > alignof(Storage)) {
			return false;
		} else {
			reset();
			new (&_storage) CallableType(std::move(callable));
			_invoke = [](void *storage, Args... args) -> ReturnType {
				return (*reinterpret_cast<CallableType *>(storage))(std::forward<Args>(args)...);
			};
			_copy = [](void *destination, const void *source) {
				new (destination) CallableType(*reinterpret_cast<const CallableType *>(source));
			};
			_move = [](void *destination, void *source) {
				new (destination)
				    CallableType(std::move(*reinterpret_cast<CallableType *>(source)));
			};
			_destroy = [](void *storage) {
				reinterpret_cast<CallableType *>(storage)->~CallableType();
			};
			return true;
		}
	}

	template <typename Instance> struct BoundMethod {
		Instance *instance = nullptr;
		ReturnType (Instance::*method)(Args...) = nullptr;

		ReturnType operator()(Args... args) const {
			if constexpr (std::is_void_v<ReturnType>) {
				(instance->*method)(std::forward<Args>(args)...);
			} else {
				return (instance->*method)(std::forward<Args>(args)...);
			}
		}
	};

	template <typename Instance> struct BoundConstMethod {
		const Instance *instance = nullptr;
		ReturnType (Instance::*method)(Args...) const = nullptr;

		ReturnType operator()(Args... args) const {
			if constexpr (std::is_void_v<ReturnType>) {
				(instance->*method)(std::forward<Args>(args)...);
			} else {
				return (instance->*method)(std::forward<Args>(args)...);
			}
		}
	};

	template <typename Instance>
	static BoundMethod<Instance> bind(Instance *instance, ReturnType (Instance::*method)(Args...)) {
		return BoundMethod<Instance>{instance, method};
	}

	template <typename Instance>
	static BoundConstMethod<Instance>
	bind(const Instance *instance, ReturnType (Instance::*method)(Args...) const) {
		return BoundConstMethod<Instance>{instance, method};
	}

	void reset() {
		if (_destroy != nullptr) {
			_destroy(&_storage);
		}
		_invoke = nullptr;
		_copy = nullptr;
		_move = nullptr;
		_destroy = nullptr;
	}

	explicit operator bool() const {
		return _invoke != nullptr;
	}

	ReturnType operator()(Args... args) const {
		if constexpr (std::is_void_v<ReturnType>) {
			_invoke(
			    const_cast<void *>(static_cast<const void *>(&_storage)),
			    std::forward<Args>(args)...
			);
		} else {
			return _invoke(
			    const_cast<void *>(static_cast<const void *>(&_storage)),
			    std::forward<Args>(args)...
			);
		}
	}

  private:
	using Storage = std::aligned_storage_t<StorageSize, alignof(std::max_align_t)>;
	using Invoke = ReturnType (*)(void *, Args...);
	using Copy = void (*)(void *, const void *);
	using Move = void (*)(void *, void *);
	using Destroy = void (*)(void *);

	void copyFrom(const LinkCallback &other) {
		if (other._copy == nullptr) {
			return;
		}
		other._copy(&_storage, &other._storage);
		_invoke = other._invoke;
		_copy = other._copy;
		_move = other._move;
		_destroy = other._destroy;
	}

	void moveFrom(LinkCallback &other) {
		if (other._move == nullptr) {
			return;
		}
		other._move(&_storage, &other._storage);
		_invoke = other._invoke;
		_copy = other._copy;
		_move = other._move;
		_destroy = other._destroy;
		other.reset();
	}

	mutable Storage _storage;
	Invoke _invoke = nullptr;
	Copy _copy = nullptr;
	Move _move = nullptr;
	Destroy _destroy = nullptr;
};
