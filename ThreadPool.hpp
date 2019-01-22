#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <functional>
#include <future>
#include <queue>

class ThreadPool {
private:
    
    struct WorkerQueue {
        // the task queue
        std::queue< std::packaged_task<void()> > tasks;
        
        // synchronization
        std::mutex queue_mutex;
        std::condition_variable condition;
    };
    
    // need to keep track of threads so we can join them
    std::vector< std::thread > workers;
    
    // queues
    std::vector< WorkerQueue > queues;
    
    bool stop;
    
    std::atomic<size_t> round_robin_idx = 0;
    
public:
    // the constructor just launches some amount of workers
    explicit ThreadPool(size_t workers) : stop(false)
    {
        for(size_t i = 0;i < workers; ++i)
        {
            this->queues.emplace_back(
                WorkerQueue()
            );
            this->workers.emplace_back(
                [this,i]
                {
                    for(;;)
                    {
                        std::packaged_task<void()> task;

                        {
                            std::unique_lock<std::mutex> lock(this->queues[i].queue_mutex);
                            this->queues[i].condition.wait(lock,
                                [this,i]{ return this->stop || !this->queues[i].tasks.empty(); }
                            );
                            if(this->stop && this->queues[i].tasks.empty())
                            {
                                return;
                            }
                            task = std::move(this->queues[i].tasks.front());
                            this->queues[i].tasks.pop();
                        }

                        task();
                    }
                }
            );
        }
    }

    // stop the pool before deleting
    ~ThreadPool()
    {
        this->stopThreadPool(true);
    }

    // stop the pool
    void stopThreadPool(bool finishTasks = false)
    {
        if (!this->stop)
        {
            this->stop = true;
            for(size_t i = 0; i < this->queues.size(); ++i)
            {
                {
                    std::unique_lock<std::mutex> lock(this->queues[i].queue_mutex);
                    if(!finishTasks)
                    {
                        std::queue< std::packaged_task<void()> > empty;
                        std::swap(this->queues[i].tasks, empty);
                    }
                }
                this->queues[i].condition.notify_all();
            }
        }

        for(auto& x : workers)
        {
            if(x.joinable())
            {
                x.join();
            }
        }
    }
    
    // add new work item to the pool
    template<class F, class... Args>
    decltype(auto) enqueue(F&& f, Args&&... args)
    {
        using return_type = std::invoke_result_t<F, Args...>;

        std::packaged_task<return_type()> task(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        // even if the whole thing is not atomic, the worst thing to happen,
        // is that the first worker gets some more work to do
        size_t chosen_worker = ++(this->round_robin_idx);
        if (chosen_worker >= this->workers.size())
        {
            this->round_robin_idx = 0;
            chosen_worker = 0;
        }

        std::future<return_type> res = task.get_future();
        {
            std::unique_lock<std::mutex> lock(this->queues[chosen_worker].queue_mutex);

            // don't allow enqueueing after stopping the pool
            if (stop)
            {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }

            this->queues[chosen_worker].tasks.emplace(std::move(task));
        }
        this->queues[chosen_worker].condition.notify_one();
        return res;
    }
};
#endif
