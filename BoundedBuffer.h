#ifndef BoundedBuffer_h
#define BoundedBuffer_h

#include <stdio.h>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;

/* The buffer of requests that patients will push to and workers will pop from */
class BoundedBuffer
{
private:
    // max number of items in the buffer
    int cap;

    queue<vector<char>> q;
    /* The queue of items in the buffer.
     Note that each item a sequence of characters that is best represented by a vector<char> for 2 reasons:
     1. An STL std::string cannot keep binary/non-printables
     2. The other alternative is keeping a char* for the sequence and an integer length (i.e., the items can be of variable length).
     While this would work, it is clearly more tedious
    */

    // add necessary synchronization variables and data structures
    mutex mtx;
    // over for full queue, under for empty queue
    condition_variable cv_over, cv_under;

public:
    BoundedBuffer(int _cap)
    {
        cap = _cap;
    }

    ~BoundedBuffer()
    {
    }

    void push(char *data, int len)
    {
        // 1. Wait until there is room in the queue (i.e., queue length is less than cap)
        unique_lock<mutex> lck(mtx);
        cv_over.wait(lck, [this]
                     { return q.size() < cap; });

        // 2. Convert the incoming byte sequence given by data and len into a vector<char>
        vector<char> back(data, data + len);

        // 3. Then push the vector at the end of the queue
        q.push(back);
        cv_under.notify_one();

        return;
    }

    int pop(char *buf, int bufcap)
    {
        // 1. Wait until the queue has at least 1 item
        unique_lock<mutex> lck(mtx);
        cv_under.wait(lck, [this]
                      { return q.size() > 0; });

        // 2. pop the front item of the queue. The popped item is a vector<char>
        vector<char> front = q.front();
        q.pop();

        // 3. Convert the popped vector<char> into a char*, copy that into buf, make sure that vector<char>'s length is <= bufcap
        assert(front.size() <= bufcap);
        memcpy(buf, &front[0], front.size());
        cv_over.notify_one();

        // 4. Return the vector's length to the caller so that he knows many bytes were popped
        return front.size();
    }

    bool empty()
    {
        return (q.size() == 0) ? true : false;
    }
};

#endif /* BoundedBuffer_ */
