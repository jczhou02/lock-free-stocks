# Implement a real-time Stock trading engine for matching Stock Buys with Stock Sells.
## The Goal:
- simulate a real time Stock Order Booking system, capable of handling multiple stockbrokers and real life applications
- addOrder() , matchOrder() to maintain our global Stock Order Book system- involving lock-free coding strategies and lock-free data structures
- ‘matchOrder’ function with a time-complexity of O(n), where 'n' is the number of orders in the Stock order book.
## Strategy:
- Support 1024 stock types as a number between 0-1024
- define 'Order' struct (properties:  ‘Order Type’ (Buy or Sell), ‘Ticker Symbol’, ‘Quantity’, ‘Price’)
- attempt to implement lock-free multi-producer, single-consumer (MPSC) queue using atomic operations and a two-phase commit (reservation and commit)
- keep two extra containers for buy / sell orders- which can help either refine matching criteria for more robust sorting + future scaling

### Potential Improvements
- [ ] Queue Overflow Handling:
The code currently prints an error message when a queue is full. In a production system, you might want to implement backpressure or a more graceful handling strategy to avoid order loss.
- [ ] Re-Enqueueing Orders:
After matching, you re-enqueue unmatched or partially matched orders back into the queues. This may alter the original order sequence, which might be acceptable in your simulation but is worth noting if order priority becomes critical.
- [ ] refining our waiting mechanism during the commit phase by possibly integrating a yield or backoff strategy. Instead of simply busy-waiting in loops (e.g., while (q->tail != index);), a call to sched_yield() or a short sleep could reduce CPU consumption under high contention.
- [ ] Evaluate whether the specific memory orders we use (e.g., memory_order_acquire/memory_order_release) match the intended use-cases, or if fine-tuning (or even switching to atomic_compare_exchange_strong in less-contended parts) might yield better performance or clarity.
- [ ] Study credible implementations of concurrentqueues (w/ or w/o blocking)- i.e, moodycamel, folly, etc.
