#pragma once

#include <new>
#include <cstdlib>

namespace stardustui {
template<typename T>
class vector {
public:
	vector() : items(nullptr), count(0), storage(0) {}

	~vector() {
		destroy_range(0, this->count);
		std::free(this->items);
	}

	vector(const vector& other) : items(nullptr), count(0), storage(0) {
		copy_from(other);
	}

	vector& operator=(const vector& other) {
		if (this != &other) {
			vector copy(other);
			swap(copy);
		}
		return *this;
	}

	vector(vector&& other) noexcept : items(other.items), count(other.count), storage(other.storage) {
		other.items = nullptr;
		other.count = 0;
		other.storage = 0;
	}

	vector& operator=(vector&& other) noexcept {
		if (this != &other) {
			destroy_range(0, this->count);
			std::free(this->items);
			this->items = other.items;
			this->count = other.count;
			this->storage = other.storage;
			other.items = nullptr;
			other.count = 0;
			other.storage = 0;
		}
		return *this;
	}

	bool push_back(const T& value) {
		if (this->count >= this->storage && !reserve(this->storage == 0 ? 8 : this->storage * 2)) {
			return false;
		}

		new (&this->items[this->count]) T(value);
		++this->count;
		return true;
	}

	bool reserve(int new_capacity) {
		if (new_capacity <= this->storage) {
			return true;
		}

		T* new_items = allocate_raw(new_capacity);
		if (new_items == nullptr) {
			return false;
		}

		for (int index = 0; index < this->count; ++index) {
			new (&new_items[index]) T(this->items[index]);
		}

		destroy_range(0, this->count);
		std::free(this->items);
		this->items = new_items;
		this->storage = new_capacity;
		return true;
	}

	int size() const {
		return this->count;
	}

	int capacity() const {
		return this->storage;
	}

	bool empty() const {
		return this->count == 0;
	}

	void clear() {
		destroy_range(0, this->count);
		this->count = 0;
	}

	void release_storage() {
		destroy_range(0, this->count);
		std::free(this->items);
		this->items = nullptr;
		this->count = 0;
		this->storage = 0;
	}

	T& operator[](int index) {
		return this->items[index];
	}

	const T& operator[](int index) const {
		return this->items[index];
	}

	T* at(int index) {
		if (index < 0 || index >= this->count) {
			return nullptr;
		}

		return &this->items[index];
	}

	const T* at(int index) const {
		if (index < 0 || index >= this->count) {
			return nullptr;
		}

		return &this->items[index];
	}

private:
	static T* allocate_raw(int capacity) {
		return capacity > 0 ? static_cast<T*>(std::malloc(sizeof(T) * static_cast<std::size_t>(capacity))) : nullptr;
	}

	void destroy_range(int begin, int end) {
		for (int index = begin; index < end; ++index) {
			this->items[index].~T();
		}
	}

	void copy_from(const vector& other) {
		if (other.storage == 0) {
			return;
		}

		T* new_items = allocate_raw(other.storage);
		if (new_items == nullptr) {
			return;
		}

		for (int index = 0; index < other.count; ++index) {
			new (&new_items[index]) T(other.items[index]);
		}

		this->items = new_items;
		this->count = other.count;
		this->storage = other.storage;
	}

	void swap(vector& other) noexcept {
		T* old_items = this->items;
		this->items = other.items;
		other.items = old_items;

		int old_count = this->count;
		this->count = other.count;
		other.count = old_count;

		int old_storage = this->storage;
		this->storage = other.storage;
		other.storage = old_storage;
	}

	T* items;
	int count;
	int storage;
};
}
