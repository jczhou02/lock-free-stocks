#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>


#define NUM_TICKERS 1024
#define QUEUE_CAPACITY 128  // If the capacity is a power of 2, you can replace the modulo with a bitwise AND (for example, pos & (capacity - 1)) which is often faster

// Global flag to control running state.
atomic_bool running = true;


typedef struct {
    char type;     // 'B' for Buy, 'S' for Sell
    int ticker;    // ticker index: 0 .. NUM_TICKERS-1
    int quantity;  // number of shares
    double price;  // price per share
} Order;

// (MPSC) Queue ---
// Modified implementation using a two-phase commit (reservation and commit).
typedef struct {
    Order orders[QUEUE_CAPACITY];
    atomic_int head;              // consumer index (orders already committed)
    atomic_int tail;              // commit index (orders ready to be dequeued)
    atomic_int tailReservation;   // reservation index (orders reserved by producers)
    int capacity;                 // fixed capacity (QUEUE_CAPACITY)
} MinimalQueue;


void initQueue(MinimalQueue* q, int capacity) {
    q->capacity = capacity;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    atomic_init(&q->tailReservation, 0);
}



bool enqueue(MinimalQueue* q, Order order) {
    int pos, head;

    while (1) {
        pos = atomic_load_explicit(&q->tailReservation, memory_order_acquire);
        head = atomic_load_explicit(&q->head, memory_order_acquire);
        if ((pos - head) >= q->capacity) {
            // Queue full.
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &q->tailReservation, &pos, pos + 1,
                memory_order_acq_rel, memory_order_relaxed))
        {
            break;  // Successfully reserved slot at index 'pos'.
        }
    }
    
    // write the order into the reserved slot
    q->orders[pos % q->capacity] = order;
    // Ensure that the order is fully written before publishing.
    atomic_thread_fence(memory_order_release);
    
    // wait until it's our turn to publish.
    while (atomic_load_explicit(&q->tail, memory_order_acquire) != pos)
        ; 
    
    atomic_store_explicit(&q->tail, pos + 1, memory_order_release);
    return true;
}



bool dequeue(MinimalQueue* q, Order* order) {
    int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    int tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail) {
        // Queue empty.
        return false;
    }
    *order = q->orders[head % q->capacity];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

// --- Global Order Book ---
// For each ticker, we maintain two queues: one for buy orders and one for sell orders.
MinimalQueue buyQueues[NUM_TICKERS];
MinimalQueue sellQueues[NUM_TICKERS];

    //  design simplification for our simulation. In a production system, might want to use a dynamic data structure or implement backpressure so that orders aren’t lost when the queue fills up

// Initialize all 1024 ticker queues.
void initOrderBook() {
    for (int i = 0; i < NUM_TICKERS; i++) {
        initQueue(&buyQueues[i], QUEUE_CAPACITY);
        initQueue(&sellQueues[i], QUEUE_CAPACITY);
    }
}

// --- Order Submission Function ---
void addOrder(char orderType, int ticker, int quantity, double price) {
    Order order;
    order.type = orderType;
    order.ticker = ticker;
    order.quantity = quantity;
    order.price = price;
    bool success = false;
    if (orderType == 'B') {
        success = enqueue(&buyQueues[ticker], order);
    } else if (orderType == 'S') {
        success = enqueue(&sellQueues[ticker], order);
    }
    if (!success) {
        // For simplicity, if the queue is full we print an error.
        printf("Queue for ticker %d is full. Order dropped.\n", ticker);
    } else {
        printf("Order added: %c Ticker: %d Qty: %d Price: %.2f\n", orderType, ticker, quantity, price);
    }
}

void matchOrders(int ticker) {
    Order buyOrders[QUEUE_CAPACITY];
    Order sellOrders[QUEUE_CAPACITY];
    int buyCount = 0, sellCount = 0;
    Order temp;
    
    // Drain the buy queue into a temporary array.
    while (dequeue(&buyQueues[ticker], &temp)) {
        buyOrders[buyCount++] = temp;
    }
    // Drain the sell queue into a temporary array.
    while (dequeue(&sellQueues[ticker], &temp)) {
        sellOrders[sellCount++] = temp;
    }
    
    // If either side is empty, re-enqueue the orders and exit.
    if (buyCount == 0 || sellCount == 0) {
        for (int i = 0; i < buyCount; i++) {
            enqueue(&buyQueues[ticker], buyOrders[i]);
        }
        for (int i = 0; i < sellCount; i++) {
            enqueue(&sellQueues[ticker], sellOrders[i]);
        }
        return;
    }
    
    // --- Bucket sort Sell Orders in ascending order ---
    // Since prices range from $10 to $1000, we loop over that range.
    Order sortedSell[QUEUE_CAPACITY];
    int sortedSellCount = 0;
    for (int price = 10; price <= 1000; price++) {
        for (int i = 0; i < sellCount; i++) {
            // Use integer comparison since our generated prices are integer values.
            if ((int)sellOrders[i].price == price && sellOrders[i].quantity > 0) {
                sortedSell[sortedSellCount++] = sellOrders[i];
            }
        }
    }
    
    // --- Bucket sort Buy Orders in descending order ---
    Order sortedBuy[QUEUE_CAPACITY];
    int sortedBuyCount = 0;
    for (int price = 1000; price >= 10; price--) {
        for (int i = 0; i < buyCount; i++) {
            if ((int)buyOrders[i].price == price && buyOrders[i].quantity > 0) {
                sortedBuy[sortedBuyCount++] = buyOrders[i];
            }
        }
    }
    
    // --- Two-Pointer Matching ---
    // 'sortedBuy' is in descending order (highest prices first)
    // 'sortedSell' is in ascending order (lowest prices first)
    int i = 0, j = 0;
    while (i < sortedBuyCount && j < sortedSellCount) {
        // If the current highest buy order meets or exceeds the lowest sell order...
        if (sortedBuy[i].price >= sortedSell[j].price) {
            // Determine the match quantity.
            int matchedQty = (sortedBuy[i].quantity < sortedSell[j].quantity) ?
                             sortedBuy[i].quantity : sortedSell[j].quantity;
            printf("Matched Ticker %d: %d shares at $%.2f\n", ticker, matchedQty, sortedSell[j].price);
            // Adjust quantities.
            sortedBuy[i].quantity -= matchedQty;
            sortedSell[j].quantity -= matchedQty;
            
            // If a buy order is completely filled, move to the next.
            if (sortedBuy[i].quantity == 0) {
                i++;
            }
            // Similarly, if a sell order is completely filled, move to the next.
            if (sortedSell[j].quantity == 0) {
                j++;
            }
        } else {
            // Since buy orders are sorted descending,
            // if the current buy order is too low, none of the remaining ones will match.
            break;
        }
    }
    
    // --- Re-enqueue remaining (unmatched or partially matched) orders ---
    for (int k = 0; k < sortedBuyCount; k++) {
        if (sortedBuy[k].quantity > 0) {
            enqueue(&buyQueues[ticker], sortedBuy[k]);
        }
    }
    for (int k = 0; k < sortedSellCount; k++) {
        if (sortedSell[k].quantity > 0) {
            enqueue(&sellQueues[ticker], sortedSell[k]);
        }
    }
}


// --- Thread Functions ---
// Producer thread: simulates stockbrokers placing random orders.
void* producerThread(void* arg) {
    unsigned int seed = time(NULL) ^ (uintptr_t)pthread_self();
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        char orderType = (rand_r(&seed) % 2 == 0) ? 'B' : 'S';
        int ticker = rand_r(&seed) % NUM_TICKERS;
        int quantity = (rand_r(&seed) % 500) + 1;  // quantity between 1 and 500
        double price = ((rand_r(&seed) % 991) + 10); // price between $10 and $1000
        addOrder(orderType, ticker, quantity, price);
        usleep(50000); // sleep 50ms
    }
    return NULL;
}

// Matcher thread: periodically scans all tickers to match orders.
void* matcherThread(void* arg) {
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        for (int ticker = 0; ticker < NUM_TICKERS; ticker++) {
            matchOrders(ticker);
        }
        usleep(500000); // sleep 500ms
    }
    return NULL;
}

// Usage: ./lockfreestocks [simulation_time_seconds] [num_stockbrokers]
int main(int argc, char* argv[]) {
    // Default values
    int simulationTime = 2;    
    int numProducers = 3;   
    
    if (argc >= 2) {
        simulationTime = atoi(argv[1]);
        if (simulationTime <= 0)
            simulationTime = 2;
    }
    
    if (argc >= 3) {
        numProducers = atoi(argv[2]);
        if (numProducers <= 0)
            numProducers = 3;
    }
    
    initOrderBook();
    
    // Create producer threads (simulate stockbrokers).
    pthread_t *producers = malloc(numProducers * sizeof(pthread_t));
    for (int i = 0; i < numProducers; i++) {
        pthread_create(&producers[i], NULL, producerThread, NULL);
    }
    
    pthread_t matcher;
    pthread_create(&matcher, NULL, matcherThread, NULL);
    
    sleep(simulationTime);
    
    atomic_store_explicit(&running, false, memory_order_release);
    
    for (int i = 0; i < numProducers; i++) {
        pthread_join(producers[i], NULL);
    }
    free(producers);
    
    pthread_join(matcher, NULL);
    
    printf("Simulation complete.\n");
    return 0;
}
