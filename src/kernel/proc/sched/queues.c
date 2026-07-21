#include <queues.h>


thread_queue_init_out thread_queue_init(uint64_t type){

    thread_queue_init_out result = {0};

    thread_queue *q = kcalloc(1, sizeof(thread_queue));
    if(q == NULL){return result ;}
    q->type = type;
    q->thread_count = 0;
    q->head = NULL;
    q->tail = NULL;

    result.success = true;
    result.q = q;
    return result;



}



uint64_t thread_queue_push(thread_queue *q, thread *t){

    if(q == NULL || t == NULL)return 0;
    if(t->owner_queue != NULL){ return 0; }

    t->q_thread_next = NULL;
    t->q_thread_prev = NULL;

    

    if(q->thread_count == 0){

        q->head = t;
        q->tail = t;
        t->owner_queue = q;
        q->thread_count++;

        return 1;
        
    }

    //else
    q->tail->q_thread_next = t;
    t->q_thread_prev = q->tail;
    q->tail = t;
    t->owner_queue = q;
    q->thread_count++;
    
    return 1;
     


}


thread_queue_pop_out thread_queue_pop(thread_queue *q){

    thread_queue_pop_out result = {0};

    if(q == NULL){return result;}

    if(q->thread_count == 0){return result;}

    thread *t = q->head;
    q->head = t->q_thread_next;

    //in case of one elemnt in queue
    if(q->head != NULL){

        q->head->q_thread_prev = NULL;

    }else{

        q->tail = NULL;
    }

    t->q_thread_next = NULL;
    t->q_thread_prev = NULL;
    t->owner_queue = NULL;
    q->thread_count--;

    result.popped_t = t;
    result.success = true;

    return result;


}



thread_queue_remove_out thread_queue_remove(thread_queue *q, thread *t){

    thread_queue_remove_out result = {0};

    if(q == NULL || t == NULL){return result;}
    if(t->owner_queue != q){return result;}


    //empty queue
    if(q->thread_count == 0)return result;

    // pointer surgery so we get O(1)

    //head case
    if(t->q_thread_prev == NULL){

        q->head = t->q_thread_next;
        if(t->q_thread_next != NULL){
            t->q_thread_next->q_thread_prev = NULL;
        }

        


    }

    //tail case
    if(t->q_thread_next == NULL){

        q->tail = t->q_thread_prev;
        if(t->q_thread_prev != NULL){
            t->q_thread_prev->q_thread_next = NULL; 
        }

        
    }

    if(t->q_thread_next != NULL && t->q_thread_prev != NULL){

        
        t->q_thread_next->q_thread_prev = t->q_thread_prev;
        t->q_thread_prev->q_thread_next = t->q_thread_next;

        

    }

    t->q_thread_next = NULL;
    t->q_thread_prev = NULL;
    t->owner_queue = NULL;
    q->thread_count--;

    result.removed_thread = t;
    result.queue_removed_from = q;
    result.success = true;

    return result;

    
    
}