#include <cstdint>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <xmmintrin.h>

#ifdef __linux__
#include <x86intrin.h>
#else
#include <intrin.h>
#endif

#define TRACK_LENGTH 0

#define CACHE_LINE_SIZE 64
#define ELEMENT_SIZE 4
#define REPORT_INTERVAL 10000000

double freq_GHz = 2.4;

inline void yield ()
{
    _mm_pause();
}

struct elem
{
    int32_t seq;

#if (ELEMENT_SIZE > 4)
    char filler[ELEMENT_SIZE - 4];
#endif
    static void* operator new[](size_t sz)
    {
        return _mm_malloc(sz * sizeof(elem), CACHE_LINE_SIZE);
    }

    static void operator delete [](void * p)
    {
        _mm_free(p);
    }
};

struct counts
{
    uint64_t received_count;
    uint64_t lost_count;
#if TRACK_LENGTH
    uint64_t sum_len;
    uint64_t min_len;
    uint64_t max_len;
#endif

    counts() {}
    counts(uint64_t received_count, uint64_t lost_count) : received_count(received_count), lost_count(lost_count) {}
#if TRACK_LENGTH
    void set_len(uint64_t sum_len, uint64_t min_len, uint64_t max_len)
    {
        this->sum_len = sum_len;
        this->min_len = min_len;
        this->max_len = max_len;
    }
#endif
};

template<typename E> class Queue
{
public:
    virtual void write(const E& elem) = 0;
    virtual void read(E& elem) = 0;
#if TRACK_LENGTH
    uint64_t sum_len;
    uint64_t min_len;
    uint64_t max_len;
#endif

    Queue()
    {
#if TRACK_LENGTH
        init_len();
#endif
    }

#if TRACK_LENGTH
    void init_len()
    {
        sum_len = 0;
        min_len = UINT64_MAX;
        max_len = 0;
    }

    void update_len(uint64_t len)
    {
        sum_len += len;
        if (len < this->min_len) min_len = len;
        if (len > this->max_len) max_len = len;
    }
#endif
};

template<typename E> class StandardQueue : public Queue<E>
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<E> queue;
    const size_t size;

public:
    StandardQueue(size_t size) : size (size)
    {
    }

    void write(const E& elem)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.size() >= size)
                return;
            queue.push_back(elem);
        }
        cv.notify_one();
    }

    void read(E& elem)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {return ! queue.empty(); });
#if TRACK_LENGTH
        this->update_len(queue.size());
#endif
        elem = queue.front();
        queue.pop_front();
    }
};

template<typename E> class CircularQueue : public Queue<E>
{
    std::mutex mutex;
    std::condition_variable cv;
    E * const queue;
    const size_t size;
    volatile size_t read_ptr;
    volatile size_t write_ptr;

public:
    CircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
    }
    
    ~CircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            size_t w = write_ptr;
            size_t nw = w + 1;
            if (nw >= size) nw = 0;
            if (nw == read_ptr) {
                return;
            }
            queue[w] = elem;
            write_ptr = nw;
        }
        cv.notify_one();
    }

    void read(E& elem)
    {
        std::unique_lock<std::mutex> lock(mutex);
        size_t r = read_ptr;
        cv.wait(lock, [this, r] {return r != write_ptr; });
        elem = queue[r];
#if TRACK_LENGTH
        size_t w = write_ptr;
        size_t len = (w >= r ? w - r : w + size - r);
        this->update_len(len);
#endif        
        if (++r == size) r = 0;
        read_ptr = r;
    }
};

template<typename E> class NoWaitCircularQueue : public Queue<E>
{
    std::mutex mutex;
    E * const queue;
    const size_t size;
    volatile size_t read_ptr;
    volatile size_t write_ptr;

public:
    NoWaitCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
    }

    ~NoWaitCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == read_ptr) {
            return;
        }
        queue[w] = elem;
        write_ptr = nw;
    }

    void read(E& elem)
    {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                size_t r = read_ptr;
                if (r != write_ptr) {
                    elem = queue[r];
#if TRACK_LENGTH
                    size_t w = write_ptr;
                    size_t len = (w >= r ? w - r : w + size - r);
                    this->update_len(len);
#endif        
                    if (++r == size) r = 0;
                    read_ptr = r;
                    return;
                }
            }
            yield();
        }
    }
};

class spinlock
{
    std::atomic_flag &flag;

public:
    spinlock(std::atomic_flag &flag) : flag(flag)
    {
        while (flag.test_and_set(std::memory_order_acquire)) yield();
    }

    ~spinlock()
    {
        flag.clear(std::memory_order_release);
    }
};

template<typename E> class SpinCircularQueue : public Queue<E>
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    E * const queue;
    const size_t size;
    volatile size_t read_ptr;
    volatile size_t write_ptr;

public:
    SpinCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
        flag.clear();
    }

    ~SpinCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        spinlock lock (flag);
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == read_ptr) {
            return;
        }
        queue[w] = elem;
        write_ptr = nw;
    }

    void read(E& elem)
    {
        while (true) {
            spinlock lock(flag);
            size_t r = read_ptr;
            if (r == write_ptr) {
                yield();
                continue;
            }
#if TRACK_LENGTH
            size_t w = write_ptr;
            size_t len = (w >= r ? w - r : w + size - r);
            this->update_len(len);
#endif        
            elem = queue[r];
            if (++r == size) r = 0;
            read_ptr = r;
            return;
        }
    }
};

template<typename E, size_t A1=1, size_t A2=1> class AtomicCircularQueue : public Queue<E>
{
    E * const queue;
    const size_t size;

    alignas (A1) std::atomic<size_t> read_ptr;
    alignas (A2) std::atomic<size_t> write_ptr;

public:
    AtomicCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
    }

    ~AtomicCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == read_ptr.load (std::memory_order_consume)) {
            return;
        }
        queue[w] = elem;
        write_ptr.store(nw, std::memory_order_release);
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        size_t w;
        while (r == (w = write_ptr.load (std::memory_order_acquire))) {
            yield();
        }
        elem = queue[r];
#if TRACK_LENGTH
        size_t len = (w >= r ? w - r : w + size - r);
        this->update_len(len);
#endif        
        if (++r == size) r = 0;
        read_ptr.store (r, std::memory_order_release);
    }
};

template<typename E> class AlignedAtomicCircularQueue : public Queue<E>
{
        E * const queue;
        const size_t size;
        std::atomic<size_t> read_ptr;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> write_ptr;

public:
    AlignedAtomicCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
    }

    ~AlignedAtomicCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == read_ptr.load(std::memory_order_consume)) {
            return;
        }
        queue[w] = elem;
        write_ptr.store(nw, std::memory_order_release);
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        size_t w;
        while (r == (w = write_ptr.load(std::memory_order_acquire))) {
            yield();
        }
        elem = queue[r];
#if TRACK_LENGTH
        size_t len = (w >= r ? w - r : w + size - r);
        this->update_len(len);
#endif        
        if (++r == size) r = 0;
        read_ptr.store(r, std::memory_order_release);
    }
};

template<typename E> class AlignedMoreAtomicCircularQueue : public Queue<E>
{
        E * const queue;
        const size_t size;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_ptr;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> write_ptr;

public:
    AlignedMoreAtomicCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), write_ptr(0)
    {
    }

    ~AlignedMoreAtomicCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == read_ptr.load(std::memory_order_consume)) {
            return;
        }
        queue[w] = elem;
        write_ptr.store(nw, std::memory_order_release);
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        size_t w;
        while (r == (w = write_ptr.load(std::memory_order_acquire))) {
            yield();
        }
        elem = queue[r];
#if TRACK_LENGTH
        size_t len = (w >= r ? w - r : w + size - r);
        this->update_len(len);
#endif        
        if (++r == size) r = 0;
        read_ptr.store(r, std::memory_order_release);
    }
};

template<typename E> class CachedAtomicCircularQueue : public Queue<E>
{
        E * const queue;
        const size_t size;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_ptr;
        size_t cached_write_ptr;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> write_ptr;
        size_t cached_read_ptr;

public:
    CachedAtomicCircularQueue(size_t size) : queue(new E[size]), size(size), read_ptr(0), cached_write_ptr(0), write_ptr(0), cached_read_ptr(0)
    {
    }

    ~CachedAtomicCircularQueue()
    {
        delete[] queue;
    }

    void write(const E& elem)
    {
        size_t w = write_ptr;
        size_t nw = w + 1;
        if (nw >= size) nw = 0;
        if (nw == cached_read_ptr) {
            cached_read_ptr = read_ptr.load(std::memory_order_consume);
            if (nw == cached_read_ptr) {
                return;
            }
        }
        queue[w] = elem;
        write_ptr.store(nw, std::memory_order_release);
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        if (r == cached_write_ptr) {
            size_t w;
            while (r == (w = write_ptr.load(std::memory_order_acquire))) {
                yield();
            }
            cached_write_ptr = w;
        }
        elem = queue[r];
#if TRACK_LENGTH
        size_t len = (cached_write_ptr >= r ? cached_write_ptr - r : cached_write_ptr + size - r);
        this->update_len(len);
#endif        
        if (++r == size) r = 0;
        read_ptr.store(r, std::memory_order_release);
    }
};

template<typename E> class DualArrayQueue : public Queue<E>
{
    const size_t size;
    E * volatile read_buf;
    E * volatile write_buf;

    alignas (CACHE_LINE_SIZE)
        size_t read_ptr;
    alignas (CACHE_LINE_SIZE)
        volatile size_t read_limit;
    alignas (CACHE_LINE_SIZE)
        std::atomic<bool> swap_requested;
    alignas (CACHE_LINE_SIZE)
        size_t write_ptr;

public:
    DualArrayQueue(size_t size) : size(size), read_buf(new E[size]), write_buf(new E[size]), read_ptr(0), read_limit(0), swap_requested(false), write_ptr(0)
    {
    }

    ~DualArrayQueue()
    {
        delete[] read_buf;
        delete[] write_buf;
    }

    void write(const E& elem)
    {
        E * t = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            t[w++] = elem;
        }
        if (swap_requested.load(std::memory_order_acquire)) {
            E* r = read_buf;
            read_buf = t;
            read_limit = w;
            swap_requested.store(false, std::memory_order_release);
            write_buf = r;
            w = 0;
        }
        write_ptr = w;
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        if (r >= read_limit) {
            swap_requested.store(true, std::memory_order_release);
            while (swap_requested.load(std::memory_order_acquire)) {
                yield();
        }

#if TRACK_LENGTH
            this->update_len(read_limit);
#endif
            r = 0;
        }
        elem = read_buf[r];
        read_ptr = r + 1;
    }
};

template<typename E> class DualArrayQueue2 : public Queue<E>
{
    const size_t size;
    E * volatile read_buf;
    E * volatile write_buf;

    alignas (CACHE_LINE_SIZE)
        size_t read_ptr;
    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_limit;
    alignas (CACHE_LINE_SIZE)
        size_t write_ptr;

public:
    DualArrayQueue2(size_t size) : size(size), read_buf(new E[size]), write_buf(new E[size]), read_ptr(size), read_limit(size), write_ptr(0)
    {
    }

    ~DualArrayQueue2()
    {
        delete[] read_buf;
        delete[] write_buf;
    }

    void write(const E& elem)
    {
        E * t = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            t[w++] = elem;
        }
        if (read_limit.load(std::memory_order_acquire) == 0) {
            E* r = read_buf;
            read_buf = t;
            read_limit.store(w, std::memory_order_release);
            write_buf = r;
            w = 0;
        }
        write_ptr = w;
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        if (r >= read_limit) {
            read_limit.store(0, std::memory_order_release);
            while (read_limit.load(std::memory_order_acquire) == 0) {
                yield();
            }
#if TRACK_LENGTH
            this->update_len(read_limit);
#endif
            r = 0;
        }
        elem = read_buf[r];
        read_ptr = r + 1;
    }
};

template<typename E> class DualArrayQueue3 : public Queue<E>
{
    const size_t size;
    E * const buf1;
    E * const buf2;

    alignas (CACHE_LINE_SIZE)
        size_t read_ptr;
        E * read_buf;

    alignas (CACHE_LINE_SIZE)
        size_t write_ptr;
        E * write_buf;

    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_limit;

public:
    DualArrayQueue3(size_t size) : size(size), buf1(new E[size]), buf2(new E[size]),
                                   read_ptr(size),
                                   write_ptr(0),
                                   read_limit(size)
    {
        read_buf = buf1;
        write_buf = buf2;
    }

    ~DualArrayQueue3()
    {
        delete[] buf1;
        delete[] buf2;
    }

    void write(const E& elem)
    {
        E * wb = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            wb[w++] = elem;
        }
        if (read_limit.load(std::memory_order_acquire) == 0) {
            read_limit.store(w, std::memory_order_release);
            write_buf = wb == buf1 ? buf2 : buf1;
            w = 0;
        }
        write_ptr = w;
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        E* t = read_buf;
        if (r >= read_limit) {
            read_limit.store(0, std::memory_order_release);
            t = read_buf = t == buf1 ? buf2 : buf1;
            while (read_limit.load(std::memory_order_acquire) == 0) {
                yield();
            }
#if TRACK_LENGTH
            this->update_len(read_limit);
#endif
            r = 0;
        }
        elem = t[r];
        read_ptr = r + 1;
    }
};

template<typename E> class DualArrayQueue4 : public Queue<E>
{
    const size_t size;
    E * const buf;
    std::uintptr_t diff;

    alignas (CACHE_LINE_SIZE)
        size_t read_ptr;
        const E * read_buf;

    alignas (CACHE_LINE_SIZE)
        size_t write_ptr;
        E * write_buf;

    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_limit;

public:
    DualArrayQueue4(size_t size) : size(size), buf(new E[2 * size]),
                                   read_ptr(size),
                                   write_ptr(0),
                                   read_limit(size)
    {
        read_buf = buf;
        write_buf = buf + size;
        diff = reinterpret_cast<uintptr_t> (read_buf) ^ reinterpret_cast<uintptr_t> (write_buf);
    }

    ~DualArrayQueue4()
    {
        delete[] buf;
    }

    void write(const E& elem)
    {
        E * wb = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            wb[w++] = elem;
        }
        if (read_limit.load(std::memory_order_acquire) == 0) {
            read_limit.store(w, std::memory_order_release);
            write_buf = reinterpret_cast<E*>(reinterpret_cast<uintptr_t>(wb) ^ diff);
            w = 0;
        }
        write_ptr = w;
    }

    void read(E& elem)
    {
        size_t r = read_ptr;
        const E* t = read_buf;
        if (r >= read_limit) {
            read_limit.store(0, std::memory_order_release);
            t = read_buf = reinterpret_cast<const E*>(reinterpret_cast<uintptr_t>(t) ^ diff);

            while (read_limit.load(std::memory_order_acquire) == 0) {
                yield();
            }
#if TRACK_LENGTH
            this->update_len(read_limit);
#endif
            r = 0;
        }
        elem = t[r];
        read_ptr = r + 1;
    }
};

template<typename E> class DualArrayAsyncQueue : public Queue<E>
{
    const size_t size;
    E * const buf;
    std::uintptr_t diff;

    alignas (CACHE_LINE_SIZE)
        size_t read_ptr;
    const E * read_buf;

    alignas (CACHE_LINE_SIZE)
        size_t write_ptr;
    E * write_buf;

    alignas (CACHE_LINE_SIZE)
        std::atomic<size_t> read_limit;

public:
    DualArrayAsyncQueue(size_t size) : size(size), buf(new E[size * 2]),
        read_ptr(0),
        write_ptr(0),
        read_limit(0)
    {
        read_buf = buf;
        write_buf = buf;
        diff = reinterpret_cast<uintptr_t> (buf) ^ reinterpret_cast<uintptr_t> (buf + size);
    }

    ~DualArrayAsyncQueue()
    {
        delete[] buf;
    }

    void write(const E& elem)
    {
        E * wb = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            wb[w++] = elem;
        }
        if (read_limit.load(std::memory_order_acquire) == 0) {
            read_limit.store(w, std::memory_order_release);
            write_buf = reinterpret_cast<E*>(reinterpret_cast<uintptr_t>(wb) ^ diff);
            w = 0;
        }
        write_ptr = w;
    }

    void read(E& elem)
    {
        size_t lim;

        while ((lim = read_limit.load(std::memory_order_acquire)) == 0) {
            yield();
        }
        size_t r = read_ptr;
        const E* t = read_buf;
        elem = t[r++];
        if (r == lim) {
#if TRACK_LENGTH
            this->update_len(lim);
#endif
            read_limit.store(0, std::memory_order_release);
            read_buf = reinterpret_cast<const E*>(reinterpret_cast<uintptr_t>(t) ^ diff);
            r = 0;
        }
        read_ptr = r;
    }
};

class empty_timer
{

public:
    empty_timer (int interval_ns)
    {
    }

    inline void start()
    {
    }

    inline void iteration(uint64_t i)
    {
    }
};

class hires_timer
{
    const unsigned FACTOR = 1024;
    const unsigned interval;
    uint64_t start_time;

public:
    hires_timer(int interval_ns) : interval((unsigned)round(freq_GHz * interval_ns * FACTOR))
    {
    }

    inline void start()
    {
        start_time = __rdtsc();
    }

    inline void iteration(uint64_t i)
    {
        sleep_until(interval * i / FACTOR + start_time);
   }

private:
    inline void sleep_until(uint64_t until)
    {
        while (__rdtsc() < until);
    }
};

class loop_timer
{
    int interval;
    volatile int sum;

public:
    loop_timer(int interval) : interval(- interval), sum(0)
    {
    }

    inline void start()
    {
    }

    inline void iteration(uint64_t i)
    {
        int s = 0;
        for (int j = 0; j < interval; j++) s += j*j;
        sum = s;
    }
};

template<class Q, class Timer> class DataSource
{
    Q &queue;
    Timer timer;

public:
    DataSource(Q &queue, int interval_ns) : queue(queue), timer (interval_ns)
    {
    }

    void operator()()
    {
        int32_t seq = 0;
        timer.start ();
        for (uint64_t i = 0; ; i++) {
            timer.iteration(i);
            elem e;
            e.seq = seq++;
            queue.write(e);
        }
    }
};

template<class Q> class DataSink
{
    Q &queue;
    Queue<counts> &report_queue;

public:
    DataSink(Q &queue, Queue<counts> &report_queue) : queue(queue), report_queue(report_queue)
    {
    }

    void operator()()
    {
        int32_t seq = 0;
        int64_t lost_count = 0;
        int64_t received_count = 0;
        while (true) {
            elem read_elem;
            queue.read(read_elem);

            if (received_count) {
                lost_count += read_elem.seq - seq;
            }
            seq = read_elem.seq + 1;
            ++received_count;
            if (received_count == REPORT_INTERVAL) {
                counts c(received_count, lost_count);
#if TRACK_LENGTH
                c.set_len(queue.sum_len, queue.min_len, queue.max_len);
                queue.init_len();
#endif
                report_queue.write(c);
                lost_count = received_count = 0;
            }
        }
    }

private:
    void report(int64_t received_count, int64_t lost_count)
    {
    }
};

class Reporter
{
    Queue<counts> &queue;

public:
    Reporter(Queue<counts> &queue) : queue(queue)
    {
    }

    void operator()()
    {
        auto start = std::chrono::system_clock::now();
        auto current_start = start;
#if TRACK_LENGTH
        uint64_t total_max = 0;
        uint64_t total_sum = 0;
        unsigned count = 0;
#endif

        while (true) {
            counts c;
            queue.read(c);
            auto now = std::chrono::system_clock::now();
            std::chrono::duration<double> since_start = now - start;
            std::chrono::duration<double> since_last = now - current_start;

#if TRACK_LENGTH
            if (c.lost_count == 0) {
                if (c.max_len > total_max) total_max = c.max_len;
                total_sum += c.sum_len;
                count++;
            }
#endif

            printf("%5.1f; write: %4d ns/elem; read: %4d ns/elem; received: %8zd; lost: %8zd"
#if TRACK_LENGTH
                "; Q min: %6zd; Q max: %6zd (%6zd) ; q avg: %8.1f (%8.1f)"
#endif
                "\n"
                , since_start.count()
                , (int)(since_last.count() * 1.0E9 / (c.received_count + c.lost_count))
                , (int)(since_last.count() * 1.0E9 / c.received_count)
                , c.received_count
                , c.lost_count
#if TRACK_LENGTH
                , c.min_len
                , c.max_len
                , total_max
                , c.sum_len * 1.0 / REPORT_INTERVAL
                , total_sum * 1.0 / REPORT_INTERVAL / count
#endif
            );
            current_start = now;
        }
    }
};

void set_affinity(int cpu, std::thread & t)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        exit(1);
    }
#endif
}

int interval_ns;
int size;
int srccpu;
int dstcpu;
int repcpu;

template<class Q, class Timer> void test_timer()
{
    std::cout << "Size: " << size << "; class: " << typeid (Q).name() << "; src CPU: " << srccpu << "; dst CPU: " << dstcpu << "\n";

    alignas (64) Q queue(size);
    alignas (64) AtomicCircularQueue<counts> report_queue(1000);

    alignas (64) DataSource<Q, Timer> source (queue, interval_ns);
    alignas (64) DataSink<Q> sink(queue, report_queue);
    alignas (64) Reporter reporter(report_queue);

    std::thread report_thread(reporter);
    std::thread sink_thread(sink);
    std::thread source_thread(source);

    if (srccpu >= 0) {
        set_affinity(srccpu, source_thread);
    }
    if (dstcpu >= 0) {
        set_affinity(dstcpu, sink_thread);
    }
    if (repcpu >= 0) {
        set_affinity(repcpu, report_thread);
    }
    source_thread.join();
}

template<class Q> void test()
{
    if (interval_ns > 0) {
        test_timer<Q, hires_timer>();
    }
    else if (interval_ns < 0) {
        test_timer<Q, loop_timer>();
    }
    else {
        test_timer<Q, empty_timer>();
    }
}

void calibrate()
{
    freq_GHz = 2.4;

    auto start = std::chrono::system_clock::now();
    auto end = start + std::chrono::duration<double> (5);
    uint64_t tsc0 = __rdtsc();
    double d = 1.0;
    static volatile double sum = 0.0;
    int cnt = 0;
    while (std::chrono::system_clock::now() < end) {
        for (int i = 0; i < 1000000; i++) {
            sum += 1 / d;
            d += 1;
        }
        ++cnt;
    }
    end = std::chrono::system_clock::now();
    uint64_t tsc1 = __rdtsc();
    std::chrono::duration<double> dur = end - start;
    freq_GHz = (tsc1 - tsc0) / dur.count() * 1.0E-9;
    std::cout << "RDTSC calibrated: freq = " << freq_GHz << " GHz\n";
}

int main(int argc, const char ** argv)
{
    interval_ns = 0;
    if (argc >= 2) interval_ns = atoi(argv[1]);

    size = 0;

    if (argc >= 3) size = atoi (argv[2]);
    if (size == 0) size = 100000;

    std::string type = "standard";
    if (argc >= 4) type = argv[3];

    srccpu = 2;
    dstcpu = 4;
    repcpu = 6;
    if (argc >= 5) srccpu = atoi(argv[4]);
    if (argc >= 6) dstcpu = atoi(argv[5]);
    if (argc >= 7) repcpu = atoi(argv[5]);
 
    calibrate();

    if (type == "standard") {
        test<StandardQueue<elem> >();
    }
    else if (type == "circular") {
        test<CircularQueue<elem> >();
    }
    else if (type == "nowait") {
        test<NoWaitCircularQueue<elem> >();
    }
    else if (type == "spin") {
        test<SpinCircularQueue<elem> >();
    }
    else if (type == "atomic") {
        test<AtomicCircularQueue<elem> >();
    }
    else if (type == "aligned-atomic") {
        test<AlignedAtomicCircularQueue<elem> >();
    }
    else if (type == "aligned-more-atomic") {
        test<AlignedMoreAtomicCircularQueue<elem> >();
    }
    else if (type == "cached-atomic") {
        test<CachedAtomicCircularQueue<elem> >();
    }
    else if (type == "dual") {
        test<DualArrayQueue<elem> >();
    }
    else if (type == "dual2") {
        test<DualArrayQueue2<elem> >();
    }
    else if (type == "dual3") {
        test<DualArrayQueue3<elem> >();
    }
    else if (type == "dual4") {
        test<DualArrayQueue4<elem> >();
    }
    else if (type == "dual-async") {
        test<DualArrayAsyncQueue<elem> >();
    }
    else {
        std::cout << "Unknown queue type: " << type << "\n";
    }

    return 0;
}
