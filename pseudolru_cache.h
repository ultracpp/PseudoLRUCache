/*
 * A thread-safe Pseudo-LRU (Least Recently Used) cache for the ESP32 microcontroller
 * Copyright (c) 2025 Eungsuk Jeon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PSEUDO_LRU_CACHE_H
#define PSEUDO_LRU_CACHE_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <stdlib.h>

static const char *CACHE_TAG = "PseudoLRUCache";

#define STATE_EMPTY 0
#define STATE_OCCUPIED 1
#define STATE_TOMBSTONE 2

typedef struct
{
    void *data;
    int refcount;
    int index;
} CacheValue;

typedef struct
{
    char **keys;
    CacheValue **values;
} CacheArray;

typedef struct
{
    int cache_size;
    int tree_nodes;
    int *tree;
    CacheArray cache;
    int hash_size;
    int hash_used;
    void (*value_free)(void *);
    SemaphoreHandle_t lock;
} PseudoLRUCache;

/**
 * @brief Create a new Pseudo-LRU cache.
 *
 * @param cache_size The maximum number of entries in the cache.
 * @param value_free A function to free the user-provided data when evicted.
 * @return Pointer to the created cache, or NULL on failure.
 */
PseudoLRUCache *createCache(int cache_size, void (*value_free)(void *));

/**
 * @brief Access or insert a value in the cache (thread-safe).
 * If the key exists, increments refcount (hit) and updates tree.
 * If not, evicts a victim using Pseudo-LRU and inserts (miss).
 *
 * @param cache The cache instance.
 * @param key The key (string, duplicated internally).
 * @param value The value to insert on miss.
 * @param value_size Size of the value in bytes.
 * @return CacheValue* on success, NULL on failure.
 */
CacheValue *accessCache(PseudoLRUCache *cache, const char *key, void *value, size_t value_size);

/**
 * @brief Release a CacheValue (decrements refcount).
 * Frees data if refcount reaches 0 and index is -1 (evicted).
 *
 * @param cache The cache instance.
 * @param cv The CacheValue to release.
 */
void releaseValue(PseudoLRUCache *cache, CacheValue *cv);

/**
 * @brief Free the entire cache and all its contents.
 *
 * @param cache The cache instance.
 */
void freeCache(PseudoLRUCache *cache);

/**
 * @brief Print the current cache state for debugging (logs via ESP_LOGI).
 *
 * @param cache The cache instance.
 */
void printCacheState(PseudoLRUCache *cache);

/**
 * @brief Print the tree bits for debugging (logs via ESP_LOGI).
 *
 * @param cache The cache instance.
 */
void printTree(PseudoLRUCache *cache);

/**
 * @brief Default free function for values (uses free()).
 *
 * @param ptr Pointer to free.
 */
void freeValue(void *ptr);

#endif // PSEUDO_LRU_CACHE_H
