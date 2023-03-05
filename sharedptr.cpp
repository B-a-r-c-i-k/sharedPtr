#include <memory>

struct BaseControlBlock {
public:
	size_t shared_count = 0;
	size_t weak_count = 0;

	BaseControlBlock(size_t sc, size_t wc) : shared_count(sc), weak_count(wc) {}

	virtual void* get() = 0;
	virtual void deallocate() = 0;
	virtual void destroy() = 0;
	virtual ~BaseControlBlock() {}
};

template <typename T, typename Deleter, typename Allocator>
struct ControlBlockWithoutMakeShared : BaseControlBlock {
private:
	T* ptr = nullptr;
	Deleter deleter;
	using AllocTraits = std::allocator_traits<typename std::allocator_traits<Allocator>
		::template rebind_alloc<ControlBlockWithoutMakeShared>>;
	typename AllocTraits::allocator_type rebind_alloc;

public:
	ControlBlockWithoutMakeShared(T* ptr, size_t sc, size_t wc, const Deleter& deleter, const Allocator& alloc)
	: BaseControlBlock(sc, wc), ptr(ptr), deleter(deleter), rebind_alloc(alloc) {}

	void deallocate() {
		AllocTraits::deallocate(rebind_alloc, this, 1);
	}

	void destroy() {
		if (ptr != nullptr)
			deleter(ptr);
	}

	void* get() { return ptr; }

	~ControlBlockWithoutMakeShared() {}

};


template <typename T, typename Allocator>
struct ControlBlockWithMakeShared : BaseControlBlock {
private:
	T object;
	using AllocTraits = std::allocator_traits<typename std::allocator_traits<Allocator>
		::template rebind_alloc<ControlBlockWithMakeShared> >;
	typename AllocTraits::allocator_type rebind_alloc;

public:
	template <typename... Args>
	ControlBlockWithMakeShared(size_t shared_count, size_t weak_count, const Allocator& alloc, Args&&... args) :
		BaseControlBlock(shared_count, weak_count), object(std::forward<Args>(args)...), rebind_alloc(alloc) {}

	void* get() { return &object; }

	void deallocate() {
		AllocTraits::deallocate(rebind_alloc, this, 1);
    }

    void destroy() {
    	AllocTraits::destroy(rebind_alloc, &object);
    }

    ~ControlBlockWithMakeShared() {}
};


template <typename T>
class SharedPtr {
private:
	BaseControlBlock* cb = nullptr;

	SharedPtr(BaseControlBlock* other_cb) {
		cb = other_cb;
		if (cb != nullptr)
			cb->shared_count++;
	}

	template <typename Allocator, typename... Args>
	SharedPtr(size_t shared_count, size_t weak_count, Allocator alloc, Args&&... args) {
		using AllocTraits = std::allocator_traits<typename std::allocator_traits<Allocator>
		::template rebind_alloc<ControlBlockWithMakeShared<T, Allocator>>>;
		typename AllocTraits::allocator_type rebind_alloc = alloc;
		auto mem_alloc = AllocTraits::allocate(rebind_alloc, 1);
		cb = mem_alloc;
		AllocTraits::construct(rebind_alloc, mem_alloc, shared_count, weak_count, alloc, std::forward<Args>(args)...);
	}

	template <typename Y, typename Alloc, typename... Args >
	friend SharedPtr<Y> allocateShared(const Alloc& alloc, Args&&... args);

	template <typename Y, typename... Args >
	friend SharedPtr<Y> makeShared(Args&&... args);

public:
	SharedPtr() = default;

	SharedPtr(const SharedPtr& other_shared_ptr) : SharedPtr(other_shared_ptr.cb) {}

	template <typename Y>
	SharedPtr(const SharedPtr<Y>& other_shared_ptr) : SharedPtr(other_shared_ptr.cb) {}

	template <typename Y,
			  typename Deleter = std::default_delete<Y>,
			  typename Allocator = std::allocator<Y>>
	explicit SharedPtr(Y* ptr, Deleter deleter = Deleter(), [[maybe_unused]] Allocator alloc = Allocator()) {
		using AllocTraits = std::allocator_traits<typename std::allocator_traits<Allocator>
		::template rebind_alloc<ControlBlockWithoutMakeShared<Y, Deleter, Allocator>>>;
		typename AllocTraits::allocator_type rebind_alloc;
		cb = AllocTraits::allocate(rebind_alloc, 1);
		new (cb) ControlBlockWithoutMakeShared<Y, Deleter, Allocator>(ptr, 1, 0, deleter, rebind_alloc);
	}

	template <typename Y>
	SharedPtr(SharedPtr<Y>&& other_shared_ptr) : cb(other_shared_ptr.cb) {
		other_shared_ptr.cb = nullptr;
	}

	SharedPtr& operator=(SharedPtr&& other_shared_ptr) {
		SharedPtr new_ptr = std::move(other_shared_ptr);
		swap(new_ptr);
		return *this;
	}

	template <typename Y>
	SharedPtr& operator=(SharedPtr<Y>&& other_shared_ptr) {
		SharedPtr new_ptr = std::move(other_shared_ptr);
		swap(new_ptr);
		return *this;
	}


	SharedPtr& operator=(const SharedPtr& other_shared_ptr) {
		SharedPtr tmp_shared_ptr = other_shared_ptr;
		swap(tmp_shared_ptr);
		return *this;
	}

	template <typename Y>
	SharedPtr& operator=(const SharedPtr<Y>& other_shared_ptr) {
		SharedPtr tmp_shared_ptr = other_shared_ptr;
		swap(tmp_shared_ptr);
		return *this;
	}

	template <typename Y>
	void swap(SharedPtr<Y>& other_shared_ptr) {
		std::swap(cb, other_shared_ptr.cb);
	}

	size_t use_count() const {
		return cb != nullptr ? cb->shared_count : 0;
	}

	T* get() const {
		if (cb == nullptr)
			return nullptr;
		return reinterpret_cast<T*>(cb->get());
	}

	T& operator*() const {
		return *get();
	}

	T* operator->() const {
		return get();
	}

	void reset() {
		*this = SharedPtr();
	}

	template<typename Y>
	void reset(Y* ptr) {
		*this = SharedPtr<T>(ptr);
	}

	~SharedPtr() {
        if (cb == nullptr)
        	return;
        --(cb->shared_count);
        if (cb->shared_count == 0) {
        	cb->destroy();
        	if (cb->weak_count == 0)
        		cb->deallocate();
        }
    }

    template <typename Y>
	friend class SharedPtr;

	template <typename Y>
	friend class WeakPtr;
};

template <typename Y, typename Alloc = std::allocator<Y>, typename... Args >
SharedPtr<Y> allocateShared(const Alloc& alloc, Args&&... args) {
	return SharedPtr<Y>(1, 0, alloc, std::forward<Args>(args)...);
}

template <typename Y, typename... Args >
SharedPtr<Y> makeShared(Args&&... args) {
	return allocateShared<Y>(std::allocator<Y>(), std::forward<Args>(args)...);
}

template <typename T>
class WeakPtr {
private:
	BaseControlBlock* cb = nullptr;

	WeakPtr(BaseControlBlock* other_cb) {
		cb = other_cb;
		if (cb != nullptr)
			cb->weak_count++;
	}

	template <typename Y>
	void swap(WeakPtr<Y>& other_weak_ptr) {
		std::swap(cb, other_weak_ptr.cb);
	}

public:

	WeakPtr() = default;

	WeakPtr(const WeakPtr& other_weak_ptr) : WeakPtr(other_weak_ptr.cb) {}

	template <typename Y>
	WeakPtr(const SharedPtr<Y>& other_shared_ptr) : WeakPtr(other_shared_ptr.cb) {}

	template <typename Y>
	WeakPtr(const WeakPtr<Y>& other_weak_ptr) : WeakPtr(other_weak_ptr.cb) {}

	WeakPtr(WeakPtr&& other_weak_ptr) : cb(other_weak_ptr.cb) {
		other_weak_ptr.cb = nullptr;
	}

	template <typename Y>
	WeakPtr(WeakPtr<Y>&& other_weak_ptr) : cb(other_weak_ptr.cb) {
		other_weak_ptr.cb = nullptr;
	}

	WeakPtr& operator=(WeakPtr&& other_weak_ptr) {
		WeakPtr new_ptr = other_weak_ptr;
		swap(new_ptr);
		return *this;
	}

	WeakPtr& operator=(const WeakPtr& other_weak_ptr) {
		WeakPtr tmp_weak_ptr = other_weak_ptr;
		swap(tmp_weak_ptr);
		return *this;
	}

	WeakPtr& operator=(const SharedPtr<T>& other_shared_ptr) {
		WeakPtr tmp_shared_ptr = other_shared_ptr;
		swap(tmp_shared_ptr);
		return *this;
	}


	template <typename Y>
	WeakPtr& operator=(const WeakPtr<Y>& other_weak_ptr) {
		WeakPtr tmp_weak_ptr = other_weak_ptr;
		swap(tmp_weak_ptr);
		return *this;
	}

	template <typename Y>
	WeakPtr& operator=(const SharedPtr<Y>& other_shared_ptr) {
		WeakPtr tmp_shared_ptr = other_shared_ptr;
		swap(tmp_shared_ptr);
		return *this;
	}

	size_t use_count() const {
		return cb != nullptr ? cb->shared_count : 0;
	}

	bool expired() const {
		return use_count() == 0;
	}

	SharedPtr<T> lock() const {
		return !expired() ? SharedPtr<T>(cb) : SharedPtr<T>();
	}

	~WeakPtr() {
		if (cb == nullptr)
			return;
		--(cb->weak_count);
		if (use_count() + cb->weak_count == 0)
			cb->deallocate();
	}

	template <typename Y>
	friend class WeakPtr;
};


