#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <functional>
#include <future>
#include <queue>

class ThreadPool {
private:
    
    struct WorkerQueue {
        // the task queue
        std::queue< std::function<void()> > tasks;
        
        // synchronization
        std::mutex queue_mutex;
        std::condition_variable condition;
    };
    
    // need to keep track of threads so we can join them
    size_t pool_size;
    std::vector< std::thread > workers;
    
    // queues
    std::vector< std::unique_ptr<WorkerQueue> > queues;
    
    bool stop;
    
    std::atomic<size_t> round_robin_idx = 0;
    
public:
    // the constructor just launches some amount of workers
    explicit ThreadPool(size_t workers) : stop(false), pool_size(workers)
    {
        this->queues.reserve(workers);
        this->workers.reserve(workers);
        for(size_t i = 0;i < workers; ++i)
        {
            this->queues.emplace_back(
                std::move(std::make_unique<WorkerQueue>())
            );
            this->workers.emplace_back(
                [this,i]
                {
                    for(;;)
                    {
                        std::function<void()> task;

                        {
                            std::unique_lock<std::mutex> lock(this->queues[i]->queue_mutex);
                            this->queues[i]->condition.wait(lock,
                                [this,i]{ return this->stop || !this->queues[i]->tasks.empty(); }
                            );
                            if(this->stop && this->queues[i]->tasks.empty())
                            {
                                return;
                            }
                            task = std::move(this->queues[i]->tasks.front());
                            this->queues[i]->tasks.pop();
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
                    std::unique_lock<std::mutex> lock(this->queues[i]->queue_mutex);
                    if(!finishTasks)
                    {
                        std::swap(this->queues[i]->tasks, std::queue< std::function<void()> >());
                    }
                }
                this->queues[i]->condition.notify_all();
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
        // don't allow enqueueing after stopping the pool
        if (stop)
        {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }
        
        // even if the whole thing is not atomic, the worst thing to happen,
        // is that the first worker gets some more work to do
        size_t chosen_worker = ++(this->round_robin_idx);
        if (chosen_worker >= this->workers.size())
        {
            this->round_robin_idx = 0;
            chosen_worker = 0;
        }

        using return_type = typename std::result_of<F(Args...)>::type;

        std::shared_ptr<std::promise<return_type>> prom = std::make_shared<std::promise<return_type>>();
        std::future<return_type> res = prom->get_future();
        
        std::function<return_type()> fnc = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::unique_lock<std::mutex> lock(this->queues[chosen_worker]->queue_mutex);

            this->queues[chosen_worker]->tasks.emplace(
                [prom = std::move(prom), fnc = std::move(fnc)]() mutable {
					try {
						__tp_invoke<return_type>(prom, fnc);
					}
					catch (...) {
						try {
							prom->set_exception(std::current_exception());
						}
						catch (...) {} // set_exception() may throw too
					}
                }
            );
        }
        this->queues[chosen_worker]->condition.notify_one();
        return res;
    }

    size_t getPoolSize() {
        return this->pool_size;
    }
};

template <typename T>
typename std::enable_if<!std::is_same<T, void>::value>::type __tp_invoke(std::shared_ptr<std::promise<T>>& prom, std::function<T()>& task)
{
    prom->set_value(task());
}
template <typename T>
typename std::enable_if<std::is_same<T, void>::value>::type __tp_invoke(std::shared_ptr<std::promise<T>>& prom, std::function<T()>& task)
{
    task();
    prom->set_value();
}
#endif
