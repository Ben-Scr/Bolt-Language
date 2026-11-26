#pragma once

// The checker owns all mutable semantic state. Requests accepted while idle are
// first in its FIFO. While busy, the reader can serve completion independently.
class AnalysisQueue {
    struct Entry { J message; std::size_t bytes; };
    std::deque<Entry> entries;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t queued_bytes=0;
    bool working=false, closing=false;
    std::thread worker;

public:
    explicit AnalysisQueue(std::function<void(const J&)> handle)
        :worker([this,handle=std::move(handle)]{
            std::unique_lock lock(mutex);
            for(;;){
                changed.wait(lock,[&]{return closing||!entries.empty();});
                if(entries.empty())return;
                auto entry=std::move(entries.front());
                entries.pop_front();
                queued_bytes-=entry.bytes;
                working=true;
                changed.notify_all();
                lock.unlock();
                handle(entry.message);
                lock.lock();
                working=false;
            }
        }){}
    ~AnalysisQueue(){
        {std::lock_guard lock(mutex);closing=true;}
        changed.notify_one();
        worker.join();
    }
    bool idle(){
        std::lock_guard lock(mutex);
        return !working&&entries.empty();
    }
    void push(J message,std::size_t bytes){
        std::unique_lock lock(mutex);
        changed.wait(lock,[&]{return entries.size()<1024&&queued_bytes+bytes<=64*1024*1024;});
        entries.push_back({std::move(message),bytes});
        queued_bytes+=bytes;
        changed.notify_all();
    }
};
