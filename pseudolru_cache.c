/*
 * Implementation of a thread-safe Pseudo-LRU (Least Recently Used) cache for the ESP32 microcontroller
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

#include "pseudolru_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_random.h>

static void cache_lock(PseudoLRUCache *c)
{
    xSemaphoreTake(c->lock, portMAX_DELAY);
}

static void cache_unlock(PseudoLRUCache *c)
{
    xSemaphoreGive(c->lock);
}

static unsigned int next_prime(unsigned int n)
{
    while (1)
    {
        int prime = 1;

        for (unsigned int i = 2; i * i <= n; i++)
        {
            if (n % i == 0)
            {
                prime = 0;
                break;
            }
        }

        if (prime)
        {
            return n;
        }

        n++;
    }
}

static unsigned int hash(const char *key, int hash_size)
{
    unsigned int h = 2166136261u;

    while (*key)
    {
        h ^= (unsigned int)*key++;
        h *= 16777619u;
    }

    return h % hash_size;
}

PseudoLRUCache *createCache(int cache_size, void (*value_free)(void *))
{
    PseudoLRUCache *cache = (PseudoLRUCache *)malloc(sizeof(PseudoLRUCache));
    if (!cache)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate cache");
        return NULL;
    }

    cache->cache_size = cache_size;
    cache->tree_nodes = cache_size - 1;
    cache->tree = (int *)malloc(cache->tree_nodes * sizeof(int));
    if (cache->tree)
    {
        memset(cache->tree, 0, cache->tree_nodes * sizeof(int));
    }
    cache->cache.keys = (char **)malloc(cache_size * sizeof(char *));
    if (cache->cache.keys)
    {
        memset(cache->cache.keys, 0, cache_size * sizeof(char *));
    }
    cache->cache.values = (CacheValue **)malloc(cache_size * sizeof(CacheValue *));
    if (cache->cache.values)
    {
        memset(cache->cache.values, 0, cache_size * sizeof(CacheValue *));
    }
    cache->hash_size = next_prime(cache_size * 2);
    cache->hash_table = (HashEntry *)malloc(cache->hash_size * sizeof(HashEntry));
    if (cache->hash_table)
    {
        memset(cache->hash_table, 0, cache->hash_size * sizeof(HashEntry));
    }
    cache->hash_used = 0;
    cache->value_free = value_free;

    cache->lock = xSemaphoreCreateMutex();
    if (!cache->lock || !cache->tree || !cache->cache.keys || !cache->cache.values || !cache->hash_table)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate cache resources");
        free(cache->tree);
        free(cache->cache.keys);
        free(cache->cache.values);
        free(cache->hash_table);
        if (cache->lock)
        {
            vSemaphoreDelete(cache->lock);
        }
        free(cache);
        return NULL;
    }

    return cache;
}

void printTree(PseudoLRUCache *cache)
{
    char bits[64] = {0};
    for (int i = 0; i < cache->tree_nodes; i++)
    {
        bits[i] = '0' + cache->tree[i];
    }
    bits[cache->tree_nodes] = '\0';
    ESP_LOGI(CACHE_TAG, "Tree bits: %s", bits);
}

void printCacheState(PseudoLRUCache *cache)
{
    char state[256] = {0};
    int offset = 0;
    for (int i = 0; i < cache->cache_size; i++)
    {
        if (cache->cache.keys[i])
        {
            offset += snprintf(state + offset, sizeof(state) - offset, "[%d: %s, ref=%d] ",
                               i, cache->cache.keys[i], cache->cache.values[i] ? cache->cache.values[i]->refcount : 0);
        }
    }
    ESP_LOGI(CACHE_TAG, "Cache state: %s", state);
}

static void updateTree(PseudoLRUCache *cache, int index)
{
    int leaf = index + cache->tree_nodes;
    int path[64], depth = 0;

    while (leaf > 0)
    {
        int parent = (leaf - 1) / 2;
        path[depth++] = (leaf == parent * 2 + 1) ? 0 : 1;
        leaf = parent;
    }

    int node = 0;

    for (int i = depth - 1; i >= 0; i--)
    {
        int dir = path[i];

        if (dir == 0)
        {
            cache->tree[node] = 1;
            node = node * 2 + 1;
        }
        else
        {
            cache->tree[node] = 0;
            node = node * 2 + 2;
        }
    }
}

static int findReplacementIndex(PseudoLRUCache *cache)
{
    int node = 0;

    while (node < cache->tree_nodes)
    {
        if (cache->tree[node] == 0)
        {
            node = node * 2 + 1;
        }
        else
        {
            node = node * 2 + 2;
        }
    }

    return node - cache->tree_nodes;
}

static void rehash(PseudoLRUCache *cache)
{
    int old_size = cache->hash_size;
    HashEntry *old_table = cache->hash_table;
    cache->hash_size = next_prime(old_size * 2);
    cache->hash_table = (HashEntry *)malloc(cache->hash_size * sizeof(HashEntry));
    if (!cache->hash_table)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate new hash table");
        cache->hash_table = old_table;
        cache->hash_size = old_size;
        return;
    }
    memset(cache->hash_table, 0, cache->hash_size * sizeof(HashEntry));
    cache->hash_used = 0;

    for (int i = 0; i < old_size; i++)
    {
        if (old_table[i].state == STATE_OCCUPIED)
        {
            unsigned int h = hash(old_table[i].key, cache->hash_size);

            while (cache->hash_table[h].state == STATE_OCCUPIED)
            {
                h = (h + 1) % cache->hash_size;
            }

            cache->hash_table[h] = old_table[i];
            cache->hash_used++;
        }
    }

    free(old_table);
}

static void insertHash(PseudoLRUCache *cache, char *key, int idx)
{
    if ((cache->hash_used * 10) / cache->hash_size >= 7)
    {
        rehash(cache);
    }

    unsigned int h = hash(key, cache->hash_size);
    int tombstone_idx = -1;

    while (1)
    {
        if (cache->hash_table[h].state == STATE_EMPTY)
        {
            if (tombstone_idx != -1)
            {
                h = tombstone_idx;
            }

            cache->hash_table[h].key = key;
            cache->hash_table[h].cache_index = idx;
            cache->hash_table[h].state = STATE_OCCUPIED;
            cache->hash_used++;
            return;
        }
        else if (cache->hash_table[h].state == STATE_TOMBSTONE)
        {
            if (tombstone_idx == -1)
            {
                tombstone_idx = h;
            }
        }
        else if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            cache->hash_table[h].cache_index = idx;
            return;
        }

        h = (h + 1) % cache->hash_size;
    }
}

static void eraseHash(PseudoLRUCache *cache, const char *key)
{
    unsigned int h = hash(key, cache->hash_size);

    while (cache->hash_table[h].state != STATE_EMPTY)
    {
        if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            cache->hash_table[h].state = STATE_TOMBSTONE;
            cache->hash_used--;
            return;
        }

        h = (h + 1) % cache->hash_size;
    }
}

static int getCacheIndex(PseudoLRUCache *cache, const char *key)
{
    unsigned int h = hash(key, cache->hash_size);

    while (cache->hash_table[h].state != STATE_EMPTY)
    {
        if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            return cache->hash_table[h].cache_index;
        }

        h = (h + 1) % cache->hash_size;
    }

    return -1;
}

CacheValue *accessCache(PseudoLRUCache *cache, const char *key, void *value, size_t value_size)
{
    cache_lock(cache);

    int index = getCacheIndex(cache, key);

    if (index != -1)
    {
        CacheValue *cv = cache->cache.values[index];

        if (cv)
        {
            cv->refcount++;
            ESP_LOGI(CACHE_TAG, "Cache hit → key: %s in line %d ref=%d", key, index, cv->refcount);
            updateTree(cache, index);
            printTree(cache);
            printCacheState(cache);
            cache_unlock(cache);
            return cv;
        }
    }

    index = findReplacementIndex(cache);

    if (cache->cache.keys[index])
    {
        eraseHash(cache, cache->cache.keys[index]);
        free(cache->cache.keys[index]);
        cache->cache.keys[index] = NULL;
    }

    CacheValue *old = cache->cache.values[index];

    if (old)
    {
        if (old->refcount == 0)
        {
            cache->value_free(old->data);
            free(old);
        }
        else
        {
            old->index = -1; // Mark as evicted
        }
        cache->cache.values[index] = NULL;
    }

    cache->cache.keys[index] = strdup(key);
    if (!cache->cache.keys[index])
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate key");
        cache_unlock(cache);
        return NULL;
    }
    CacheValue *cv = (CacheValue *)malloc(sizeof(CacheValue));
    if (!cv)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate CacheValue");
        free(cache->cache.keys[index]);
        cache->cache.keys[index] = NULL;
        cache_unlock(cache);
        return NULL;
    }
    cv->data = malloc(value_size);
    if (!cv->data)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate data");
        free(cv);
        free(cache->cache.keys[index]);
        cache->cache.keys[index] = NULL;
        cache_unlock(cache);
        return NULL;
    }
    memcpy(cv->data, value, value_size);
    cv->refcount = 1;
    cv->index = index;
    cache->cache.values[index] = cv;

    insertHash(cache, cache->cache.keys[index], index);

    ESP_LOGI(CACHE_TAG, "Cache miss → stored key: %s in line %d ref=1", key, index);
    updateTree(cache, index);
    printTree(cache);
    printCacheState(cache);

    cache_unlock(cache);
    return cv;
}

void releaseValue(PseudoLRUCache *cache, CacheValue *cv)
{
    if (!cv)
    {
        return;
    }

    cache_lock(cache);
    cv->refcount--;

    if (cv->refcount == 0 && cv->index == -1)
    {
        cache->value_free(cv->data);
        free(cv);
    }

    cache_unlock(cache);
}

void freeCache(PseudoLRUCache *cache)
{
    for (int i = 0; i < cache->cache_size; i++)
    {
        if (cache->cache.keys[i])
        {
            eraseHash(cache, cache->cache.keys[i]);
            free(cache->cache.keys[i]);
            cache->cache.keys[i] = NULL;
        }

        CacheValue *cv = cache->cache.values[i];

        if (cv)
        {
            if (cv->refcount > 0)
            {
                ESP_LOGW(CACHE_TAG, "Warning: freeing held CacheValue at %d (ref=%d)", i, cv->refcount);
                cache->value_free(cv->data);
                free(cv);
            }
            else if (cv->refcount == 0)
            {
                cache->value_free(cv->data);
                free(cv);
            }
            cache->cache.values[i] = NULL;
        }
    }

    free(cache->cache.keys);
    free(cache->cache.values);
    free(cache->tree);
    free(cache->hash_table);

    vSemaphoreDelete(cache->lock);
    free(cache);
}

void freeValue(void *ptr)
{
    free(ptr);
}
