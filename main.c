/*
 * Test application for Pseudo-LRU Cache in ESP-IDF
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#define NUM_THREADS 8
#define OPS_PER_THREAD 1000
#define YIELD_INTERVAL 100
#define TEST_INTERVAL_MS 5000

typedef struct
{
    PseudoLRUCache *cache;
    const char **keys;
    int *values;
    int num_keys;
    SemaphoreHandle_t done_sem;
} ThreadArg;

static void threadFunc(void *arg)
{
    ThreadArg *t = (ThreadArg *)arg;

    for (int i = 0; i < OPS_PER_THREAD; i++)
    {
        int idx = esp_random() % t->num_keys;
        CacheValue *cv = accessCache(t->cache, t->keys[idx], &t->values[idx], sizeof(int));

        if (cv)
        {
            releaseValue(t->cache, cv);
        }

        if ((i + 1) % YIELD_INTERVAL == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    xSemaphoreGive(t->done_sem);
    vTaskDelete(NULL);
}

void app_main(void)
{
    SemaphoreHandle_t done_sem = xSemaphoreCreateCounting(NUM_THREADS, 0);
    if (!done_sem)
    {
        ESP_LOGE(CACHE_TAG, "Failed to create done semaphore");
        return;
    }

    const char *keys[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
    int values[] = {1, 2, 3, 4, 5, 6, 7, 8};
    int num_keys = 8;

    while (1)
    {
        ESP_LOGI(CACHE_TAG, "Starting new test cycle, free heap: %lu bytes", esp_get_free_heap_size());
        PseudoLRUCache *cache = createCache(4, freeValue);

        if (!cache)
        {
            ESP_LOGE(CACHE_TAG, "Failed to create cache");
            vSemaphoreDelete(done_sem);
            return;
        }

        ThreadArg args[NUM_THREADS];
        TaskHandle_t threads[NUM_THREADS];

        for (int i = 0; i < NUM_THREADS; i++)
        {
            args[i].cache = cache;
            args[i].keys = keys;
            args[i].values = values;
            args[i].num_keys = num_keys;
            args[i].done_sem = done_sem;

            char task_name[16];
            snprintf(task_name, sizeof(task_name), "thread_%d", i);
            if (xTaskCreate(threadFunc, task_name, 4096, &args[i], 5, &threads[i]) != pdPASS)
            {
                ESP_LOGE(CACHE_TAG, "Failed to create task %d", i);
            }
        }

        for (int i = 0; i < NUM_THREADS; i++)
        {
            xSemaphoreTake(done_sem, portMAX_DELAY);
        }

        freeCache(cache);
        ESP_LOGI(CACHE_TAG, "Test cycle completed, cache freed, free heap: %lu bytes", esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(TEST_INTERVAL_MS));
    }

    vSemaphoreDelete(done_sem);
}

// idf.py -p COM5 -b 115200 flash monitor
