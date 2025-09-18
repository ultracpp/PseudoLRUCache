# Thread-Safe Pseudo-LRU Cache for ESP32

This project implements a thread-safe Pseudo-LRU cache tailored for the ESP32 microcontroller, leveraging FreeRTOS for task management and mutexes for synchronization. It uses a binary tree-based algorithm to approximate the Least Recently Used (LRU) eviction policy.

## Features

  - **Thread-Safe**: Uses a FreeRTOS mutex (`SemaphoreHandle_t`) to ensure atomic operations on the cache, making it safe for multi-threaded access.
  - **Pseudo-LRU Algorithm**: Employs a simple binary tree to keep track of recently accessed cache lines. This method is efficient and avoids the overhead of a true LRU implementation, which would require a linked list.
  - **Dynamic Hash Table**: The cache uses a hash table with linear probing to store key-to-index mappings. It automatically **rehashes** (resizes and rebuilds the hash table) when the load factor exceeds 70% to maintain good performance.
  - **Reference Counting**: Each cached value includes a reference count. The cache will only free a value when its reference count drops to zero, even if the value has been evicted from the main cache array. This prevents use-after-free bugs in concurrent applications.
  - **Configurable**: The cache size can be specified during creation, and a custom function for freeing cached values can be provided.
  - **ESP-IDF Integration**: The code is written for the ESP-IDF framework, using its logging (`ESP_LOGI`, `ESP_LOGE`) and task management (`xTaskCreate`, `vTaskDelete`).

## How It Works

### Pseudo-LRU Eviction

The Pseudo-LRU policy is managed by a binary tree. The tree has `cache_size - 1` nodes, where each node is a single bit (0 or 1).

  - When a cache line is accessed, the tree is traversed from the root to the leaf node corresponding to that cache line's index.
  - At each step, the bit of the current node is flipped to point away from the path taken.
  - To find the **least recently used** item to evict, the tree is traversed from the root, always following the bit that has **not** been set (e.g., if the bit is 0, go left; if it's 1, go right). This path leads to the index of the next item to be replaced.

This method provides a low-overhead approximation of true LRU, which is suitable for embedded systems like the ESP32.

### Thread Safety and Reference Counting

The cache uses a mutex to protect all critical sections, specifically `accessCache`, `releaseValue`, and `freeCache`. This prevents race conditions when multiple tasks try to read from or write to the cache simultaneously.

Each `CacheValue` has a `refcount` (reference count). When a value is retrieved with `accessCache`, its `refcount` is incremented. When the calling task is done with the value, it must call `releaseValue`, which decrements the `refcount`.

  - If a value is evicted from the cache while its `refcount` is greater than zero, the value's key and its slot in the main cache array are freed, but the `CacheValue` object itself is **not** freed. Its `index` is set to `-1` to mark it as evicted.
  - The memory for the `CacheValue` object and its data is only freed when `releaseValue` is called and the `refcount` reaches zero. This guarantees that a task holding a reference to a cached item will not encounter a stale pointer.

### Hash Table

The hash table uses **linear probing** to handle collisions. It automatically grows (rehashes) when the number of entries exceeds 70% of the table's size. This prevents performance degradation as the cache fills up. The hash function used is a variant of the FNV-1a algorithm, which is fast and provides good distribution.

## API

### `PseudoLRUCache *createCache(int cache_size, void (*value_free)(void *))`

Creates and initializes a new `PseudoLRUCache` instance.

  - `cache_size`: The maximum number of items the cache can hold.
  - `value_free`: A pointer to a function that will be called to free the `data` of a `CacheValue`.

### `CacheValue *accessCache(PseudoLRUCache *cache, const char *key, void *value, size_t value_size)`

Accesses the cache with a given `key`.

  - If the key exists (**cache hit**), it returns the existing `CacheValue` and increments its `refcount`.
  - If the key does not exist (**cache miss**), a new entry is created. If the cache is full, it evicts the least recently used item first. It then allocates memory, copies the provided `value` into the cache, and returns the new `CacheValue`.
  - The returned `CacheValue` is valid until `releaseValue` is called with a zero reference count.

### `void releaseValue(PseudoLRUCache *cache, CacheValue *cv)`

Decrements the `refcount` of a `CacheValue`. If the `refcount` drops to zero, the memory for the `CacheValue` and its data is freed.

### `void freeCache(PseudoLRUCache *cache)`

Frees all resources associated with the cache. This should only be called after all tasks have released their references to cached items. If a `refcount` is still greater than zero, a warning will be logged, and the value will be freed anyway.

## Example Usage

The provided `app_main.c` demonstrates how to use the cache in a multi-threaded environment. It creates a cache and then spawns multiple tasks that concurrently access and release values. A test loop periodically creates and frees the cache to simulate a long-running, dynamic system.

```c
// Example from app_main.c
PseudoLRUCache *cache = createCache(4, freeValue);
// ...
// In a thread:
CacheValue *cv = accessCache(cache, "some_key", &some_data, sizeof(some_data));
if (cv) {
    // Use the data
    int retrieved_val = *(int *)cv->data;
    // ...
    releaseValue(cache, cv);
}
// ...
freeCache(cache);
```
