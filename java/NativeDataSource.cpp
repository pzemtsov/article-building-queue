#include "NativeDataSource.h"

#include <cstdint>
#include <thread>
#include <atomic>
#include <cmath>

#ifdef __linux__
#include <x86intrin.h>
#else
#include <intrin.h>
#endif


double freq_GHz = 2.4;

void calibrate()
{
    freq_GHz = 2.4;

    auto start = std::chrono::system_clock::now();
    auto end = start + std::chrono::duration<double>(5);
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
}

template<typename E> class DualArrayAsyncWriter
{
    E * const buf;
    const size_t size;
    std::atomic <uint32_t> & read_limit;
    size_t write_ptr;
    E * write_buf;
    std::uintptr_t diff;

public:
    DualArrayAsyncWriter(E * buf, size_t size, std::atomic <uint32_t> & read_limit)
        : buf (buf), size(size), read_limit (read_limit),
          write_ptr(0)
    {
        write_buf = buf;
        diff = reinterpret_cast<uintptr_t> (buf) ^ reinterpret_cast<uintptr_t> (buf + size);
    }

    ~DualArrayAsyncWriter()
    {
    }

    void write(E elem)
    {
        E * wb = write_buf;
        size_t w = write_ptr;
        if (w < size) {
            wb[w++] = elem;
        }
        if (read_limit.load(std::memory_order_acquire) == 0) {
            read_limit.store((uint32_t) w, std::memory_order_release);
            write_buf = reinterpret_cast<E*>(reinterpret_cast<uintptr_t>(wb) ^ diff);
            w = 0;
        }
        write_ptr = w;
    }
};

template<class Timer> class DataSource
{
    DualArrayAsyncWriter<uint32_t> writer;
    Timer timer;

public:
    DataSource(uint32_t * buf, size_t size, std::atomic<uint32_t> & read_limit, int interval_ns) : writer(buf, size, read_limit), timer(interval_ns)
    {
    }

    void operator()()
    {
        int32_t seq = 0;
        timer.start();
        for (uint64_t i = 0; ; i++) {
            timer.iteration(i);
            writer.write(seq++);
        }
    }
};

class empty_timer
{

public:
    empty_timer(int interval_ns)
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

JNIEXPORT void JNICALL Java_NativeDataSource_calibrate(JNIEnv *, jclass)
{
    calibrate();
}

JNIEXPORT void JNICALL Java_NativeDataSource_start(JNIEnv * env, jclass, jobject jbuf, jint size, jobject jread_limit_buf, jint interval)
{
    std::atomic<uint32_t> * read_limit_ptr = (std::atomic<uint32_t> *)env->GetDirectBufferAddress(jread_limit_buf);
    uint32_t * buf = (uint32_t*)env->GetDirectBufferAddress(jbuf);
    if (interval == 0)
        new std::thread(*new DataSource<empty_timer>(buf, (size_t)size, *read_limit_ptr, interval));
    else
        new std::thread(*new DataSource<hires_timer>(buf, (size_t)size, *read_limit_ptr, interval));
}
