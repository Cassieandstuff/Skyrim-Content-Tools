#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

// ── BackgroundWorker<Req, Res> ────────────────────────────────────────────────
// One dedicated worker thread that processes a queue of requests and deposits
// results into a result queue for the main thread to drain each frame.
//
// Lifetime contract:
//   - Construct on the main thread, passing the processing function.
//   - Call Submit() from the main thread (non-blocking).
//   - Call Drain() once per frame on the main thread to consume results.
//   - Destructor signals stop, drains the pending queue, and joins the thread.
//     No work is orphaned; no thread outlives the owning object.
//
// Thread model:
//   - One worker thread (single-producer → single-consumer in each direction).
//   - Pending queue:  main thread → worker (guarded by pendingMtx_).
//   - Result queue:   worker → main thread (guarded by resultsMtx_).
//   - The two mutexes are never held simultaneously — no deadlock possible.
//
// Evolving to N worker threads:
//   Replace thread_ with std::vector<std::thread> and launch N threads in the
//   constructor.  The queue logic is unchanged; results from any thread land in
//   the same result queue.  The only addition is a loop over threads in Stop().
//
// Error handling:
//   If the processing function throws, the exception is caught, logged to
//   stderr, and the worker continues processing the next request.  The failed
//   request produces no result entry.

template<typename Req, typename Res>
class BackgroundWorker {
public:
    using ProcessFn = std::function<Res(const Req&)>;

    // Construct and start the worker thread immediately.
    explicit BackgroundWorker(ProcessFn fn)
        : processFn_(std::move(fn))
        , running_(true)
        , thread_([this]{ WorkerLoop(); })
    {}

    // Non-copyable, non-movable (thread + mutexes cannot be transferred).
    BackgroundWorker(const BackgroundWorker&)            = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;
    BackgroundWorker(BackgroundWorker&&)                 = delete;
    BackgroundWorker& operator=(BackgroundWorker&&)      = delete;

    // Signals the worker to stop, joins the thread.
    // In-flight work (the single item being processed right now, if any)
    // completes normally and its result is discarded.  Pending items not yet
    // picked up by the worker are dropped without processing.
    ~BackgroundWorker() { Stop(); }

    // ── Main-thread API ───────────────────────────────────────────────────────

    // Enqueue a request.  Returns immediately; does not block.
    void Submit(Req req)
    {
        {
            std::lock_guard<std::mutex> lk(pendingMtx_);
            pending_.push(std::move(req));
        }
        pendingCv_.notify_one();
    }

    // Discard all queued-but-not-yet-started requests.
    // Useful for lightweight cancellation when the context changes (e.g. the
    // user loads a different cell before the previous one finishes streaming).
    // Does NOT cancel the single request the worker may currently be executing.
    void ClearPending()
    {
        std::lock_guard<std::mutex> lk(pendingMtx_);
        while (!pending_.empty()) pending_.pop();
    }

    // Call once per frame on the main thread.
    // Invokes callback(result) for every completed result, then clears them.
    // Results are moved out of the queue — no copies of potentially large
    // result objects.
    void Drain(std::function<void(Res)> callback)
    {
        std::lock_guard<std::mutex> lk(resultsMtx_);
        while (!results_.empty()) {
            callback(std::move(results_.front()));
            results_.pop();
        }
    }

    // Number of requests queued but not yet started.  Includes nothing that
    // is currently being processed.  Useful for debug overlays.
    int Pending() const
    {
        std::lock_guard<std::mutex> lk(pendingMtx_);
        return static_cast<int>(pending_.size());
    }

    // Number of completed results waiting to be drained.
    int Ready() const
    {
        std::lock_guard<std::mutex> lk(resultsMtx_);
        return static_cast<int>(results_.size());
    }

    // Explicit early shutdown.  Safe to call multiple times (idempotent).
    void Stop()
    {
        running_.store(false, std::memory_order_release);
        pendingCv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    // ── Worker thread ─────────────────────────────────────────────────────────

    void WorkerLoop()
    {
        while (true) {
            Req req;
            {
                std::unique_lock<std::mutex> lk(pendingMtx_);
                pendingCv_.wait(lk, [this]{
                    return !pending_.empty() ||
                           !running_.load(std::memory_order_acquire);
                });

                // Stop requested and nothing left to start — exit cleanly.
                if (!running_.load(std::memory_order_acquire) && pending_.empty())
                    return;

                req = std::move(pending_.front());
                pending_.pop();
            }
            // Process outside the lock so Submit() is never blocked by a
            // long-running task.
            try {
                Res res = processFn_(req);
                std::lock_guard<std::mutex> lk(resultsMtx_);
                results_.push(std::move(res));
            } catch (const std::exception& e) {
                fprintf(stderr, "[BackgroundWorker] task threw: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "[BackgroundWorker] task threw unknown exception\n");
            }
        }
    }

    // ── Data ──────────────────────────────────────────────────────────────────

    ProcessFn         processFn_;

    std::atomic_bool  running_;
    std::thread       thread_;      // declared after running_ — init order matters

    mutable std::mutex      pendingMtx_;
    std::condition_variable pendingCv_;
    std::queue<Req>         pending_;

    mutable std::mutex  resultsMtx_;
    std::queue<Res>     results_;
};
