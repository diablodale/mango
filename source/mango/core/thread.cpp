/*
    MANGO Multimedia Development Platform
    Copyright (C) 2012-2016 Twilight Finland 3D Oy Ltd. All rights reserved.
*/
#include <chrono>
#include <mango/core/thread.hpp>
#include "../../external/concurrentqueue/concurrentqueue.h"

using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

// ------------------------------------------------------------
// thread affinity
// ------------------------------------------------------------

// TODO: IOS, OSX, ANDROID

#if defined(MANGO_PLATFORM_LINUX) || defined(MANGO_PLATFORM_BSD)

#include <pthread.h>

    template <typename H>
    static void set_thread_affinity(H handle, int processor)
    {
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(processor, &cpuset);
        pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
    }

    static void set_current_thread_affinity(int processor)
    {
        set_thread_affinity(pthread_self(), processor);
    }

#elif defined(MANGO_PLATFORM_WINDOWS)

    template <typename H>
    static void set_thread_affinity(H handle, int processor)
    {
        SetThreadAffinityMask(handle, DWORD_PTR(1) << processor);
    }

    static void set_current_thread_affinity(int processor)
    {
        set_thread_affinity(GetCurrentThread(), processor);
    }

#else

    template <typename H>
    static void set_thread_affinity(H handle, int processor)
    {
        MANGO_UNREFERENCED_PARAMETER(handle);
        MANGO_UNREFERENCED_PARAMETER(processor);
    }

    static void set_current_thread_affinity(int processor)
    {
        MANGO_UNREFERENCED_PARAMETER(processor);
    }

#endif

namespace mango
{

    // ------------------------------------------------------------
    // TaskQueue
    // ------------------------------------------------------------

    struct TaskQueue
    {
        using Task = ThreadPool::Task;
        moodycamel::ConcurrentQueue<Task> tasks;
    };

    // ------------------------------------------------------------
    // ThreadPool
    // ------------------------------------------------------------

    ThreadPool::ThreadPool(size_t size)
    : m_queue_cache(32), m_queues(nullptr), m_threads(size)
    {
        m_queues = new TaskQueue[3];
        m_static_queue = createQueue("static", static_cast<int>(Priority::NORMAL));

        // NOTE: let OS scheduler shuffle tasks as it sees fit
        //       this gives better performance overall UNTIL we have some practical
        //       use for the affinity (eg. dependent tasks using same cache)
        const bool affinity = false;//std::thread::hardware_concurrency() > 1;
		if (affinity)
            set_current_thread_affinity(0);

        for (size_t i = 0; i < size; ++i)
        {
            m_threads[i] = std::thread([this, i] {
                thread(i);
            });

			if (affinity)
                set_thread_affinity(m_threads[i].native_handle(), int(i + 1));
        }
    }

    ThreadPool::~ThreadPool()
    {
        m_stop = true;
        m_condition.notify_all();

        for (auto& thread : m_threads)
        {
            thread.join();
        }

        deleteQueue(m_static_queue);
        delete[] m_queues;
    }

    ThreadPool& ThreadPool::getInstance()
    {
        static ThreadPool instance(std::max(std::thread::hardware_concurrency() - 1, 1U));
        return instance;
    }

    int ThreadPool::getInstanceSize()
    {
        ThreadPool& pool = getInstance();
        return pool.size();
    }

    int ThreadPool::size() const
    {
        return int(m_threads.size());
    }

    void ThreadPool::thread(size_t threadID)
    {
        auto time0 = high_resolution_clock::now();

        while (!m_stop.load(std::memory_order_relaxed))
        {
            if (dequeue_process())
            {
                // remember the last time we processed a task
                time0 = high_resolution_clock::now();
            }
            else
            {
                // don't be too eager to sleep
                auto time1 = high_resolution_clock::now();
                auto elapsed = duration_cast<milliseconds>(time1 - time0).count();
                if (elapsed >= milliseconds(2).count())
                {
                    // sleep but check what's happening after a while unless signaled
                    ++m_sleep_count;
                    std::unique_lock<std::mutex> lock(m_queue_mutex);
                    m_condition.wait_for(lock, milliseconds(32));
                    --m_sleep_count;
                }
                else
                {
                    // no work; yield and try again soon
                    std::this_thread::yield();
                }
            }
        }
    }

    void ThreadPool::enqueue(Queue* queue, std::function<void()>&& func)
    {
        queue->retain();

        Task task;
        task.queue = queue;
        task.stamp = queue->task_input_count++;
        task.barrier = queue->stamp_barrier;
        task.func = std::move(func);

        m_queues[queue->priority].tasks.enqueue(std::move(task));

        if (m_sleep_count > 0)
            m_condition.notify_one();
    }

    bool ThreadPool::dequeue_process()
    {
        // scan task queues in priority order
        for (size_t priority = 0; priority < 3; ++priority)
        {
            Task task;
            if (m_queues[priority].tasks.try_dequeue(task))
            {
                Queue* queue = task.queue;
                
                // check if the task is cancelled
                if (task.stamp > queue->stamp_cancel)
                {
                    // wait until task is not blocked by a barrier
                    while (task.barrier > queue->task_complete_count)
                        ;
                    
                    // process task
                    task.func();
                }

                ++queue->task_complete_count;
                queue->release();
                
                return true;
            }
        }

        return false;
    }

    void ThreadPool::wait(Queue* queue)
    {
        // NOTE: we might be waiting here a while if other threads keep enqueuing tasks
        while (queue->task_complete_count < queue->task_input_count)
        {
            if (!dequeue_process())
            {
                std::this_thread::yield();
            }
        }
    }

    void ThreadPool::cancel(Queue* queue)
    {
        queue->stamp_cancel = queue->task_input_count.load() - 1;
    }

    ThreadPool::Queue* ThreadPool::createQueue(const std::string& name, int priority)
    {
        Queue* queue = m_queue_cache.acquire();

        queue->pool = this;
        queue->priority = priority;
        queue->reference_count = 1;
        queue->task_input_count = 0;
        queue->task_complete_count = 0;
        queue->stamp_cancel = -1;
        queue->stamp_barrier = 0;
        queue->name = name;

        return queue;
    }

    void ThreadPool::deleteQueue(Queue* queue)
    {
        m_queue_cache.discard(queue);
    }

    // ------------------------------------------------------------
    // ConcurrentQueue
    // ------------------------------------------------------------

    ConcurrentQueue::ConcurrentQueue()
    : m_pool(ThreadPool::getInstance())
    {
        m_queue = m_pool.createQueue("none", static_cast<int>(Priority::NORMAL));
    }

    ConcurrentQueue::ConcurrentQueue(const std::string& name, Priority priority)
    : m_pool(ThreadPool::getInstance())
    {
        m_queue = m_pool.createQueue(name, static_cast<int>(priority));
    }

    ConcurrentQueue::~ConcurrentQueue()
    {
        m_queue->release();
    }

    void ConcurrentQueue::barrier()
    {
        m_queue->stamp_barrier = m_queue->task_input_count;
    }

    void ConcurrentQueue::cancel()
    {
        m_pool.cancel(m_queue);
    }

    void ConcurrentQueue::wait()
    {
        m_pool.wait(m_queue);
    }

    // ------------------------------------------------------------
    // SerialQueue
    // ------------------------------------------------------------

    SerialQueue::SerialQueue()
    : m_pool(ThreadPool::getInstance())
    {
        m_queue = m_pool.createQueue("none", static_cast<int>(Priority::NORMAL));
    }

    SerialQueue::SerialQueue(const std::string& name, Priority priority)
    : m_pool(ThreadPool::getInstance())
    {
        m_queue = m_pool.createQueue(name, static_cast<int>(priority));
    }

    SerialQueue::~SerialQueue()
    {
        m_queue->release();
    }

    void SerialQueue::cancel()
    {
        m_pool.cancel(m_queue);
    }

    void SerialQueue::wait()
    {
        m_pool.wait(m_queue);
    }

} // namespace mango
