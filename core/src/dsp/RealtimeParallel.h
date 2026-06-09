#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace guitarfx
{
  namespace rtparallel
  {
    inline void CpuRelax() noexcept
    {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
      _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
  #if defined(__x86_64__) || defined(__i386__)
      __asm volatile("pause" ::: "memory");
  #elif defined(__aarch64__) || defined(__arm__)
      __asm volatile("yield" ::: "memory");
  #endif
#endif
    }

    class DualLaneExecutor
    {
    public:
      static DualLaneExecutor &Instance()
      {
        static DualLaneExecutor instance;
        return instance;
      }

      DualLaneExecutor(const DualLaneExecutor &) = delete;
      DualLaneExecutor &operator=(const DualLaneExecutor &) = delete;

      [[nodiscard]] bool IsAvailable() const noexcept
      {
        return mWorkerStarted.load(std::memory_order_acquire);
      }

      template <typename WorkerFn, typename MainFn>
      bool Run(WorkerFn &&workerFn, MainFn &&mainFn)
      {
        if (!IsAvailable())
          return false;

        bool expected = false;
        if (!mBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
          return false;

        struct JobContext
        {
          WorkerFn *fn;
        };

        JobContext ctx{&workerFn};

        mWorkerThunk.store(&Invoke<JobContext>, std::memory_order_release);
        mWorkerContext.store(&ctx, std::memory_order_release);
        mWorkerDone.store(false, std::memory_order_release);

        {
          std::lock_guard<std::mutex> lock(mMutex);
          mGeneration.fetch_add(1, std::memory_order_relaxed);
        }
        mCv.notify_one();

        std::forward<MainFn>(mainFn)();

        while (!mWorkerDone.load(std::memory_order_acquire))
          CpuRelax();

        mBusy.store(false, std::memory_order_release);
        return true;
      }

    private:
      using WorkerThunk = void (*)(void *);

      DualLaneExecutor()
      {
        const unsigned int hw = std::thread::hardware_concurrency();
        if (hw < 2)
          return;

        try
        {
          mWorkerThread = std::thread([this]() { WorkerLoop(); });
          mWorkerStarted.store(true, std::memory_order_release);
        }
        catch (...)
        {
          mWorkerStarted.store(false, std::memory_order_release);
        }
      }

      ~DualLaneExecutor()
      {
        if (!mWorkerStarted.load(std::memory_order_acquire))
          return;

        {
          std::lock_guard<std::mutex> lock(mMutex);
          mQuit.store(true, std::memory_order_relaxed);
          mGeneration.fetch_add(1, std::memory_order_relaxed);
        }
        mCv.notify_one();

        if (mWorkerThread.joinable())
          mWorkerThread.join();
      }

      template <typename JobContext>
      static void Invoke(void *ctx)
      {
        if (!ctx)
          return;
        auto *typed = static_cast<JobContext *>(ctx);
        if (typed->fn)
          (*typed->fn)();
      }

      void WorkerLoop()
      {
        std::uint32_t lastGeneration = 0;
        while (true)
        {
          {
            std::unique_lock<std::mutex> lock(mMutex);
            mCv.wait(lock, [&]()
            {
              return mQuit.load(std::memory_order_relaxed)
                || mGeneration.load(std::memory_order_relaxed) != lastGeneration;
            });
          }

          if (mQuit.load(std::memory_order_acquire))
            break;

          lastGeneration = mGeneration.load(std::memory_order_acquire);

          auto *thunk = mWorkerThunk.load(std::memory_order_acquire);
          void *context = mWorkerContext.load(std::memory_order_acquire);
          if (thunk)
            thunk(context);

          mWorkerDone.store(true, std::memory_order_release);
        }
      }

      std::atomic<bool> mWorkerStarted{false};
      std::atomic<bool> mBusy{false};
      std::atomic<bool> mWorkerDone{false};
      std::atomic<bool> mQuit{false};

      std::atomic<WorkerThunk> mWorkerThunk{nullptr};
      std::atomic<void *> mWorkerContext{nullptr};

      std::atomic<std::uint32_t> mGeneration{0};
      std::mutex mMutex;
      std::condition_variable mCv;
      std::thread mWorkerThread;
    };

    [[nodiscard]] inline bool ShouldParallelizeStereoWork(int numSamples) noexcept
    {
      constexpr int kMinSamples = 96;
      return numSamples >= kMinSamples;
    }
  } // namespace rtparallel
} // namespace guitarfx
