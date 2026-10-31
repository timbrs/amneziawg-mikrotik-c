#include "proxy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    normal_sessions_t *t = calloc(1, sizeof(*t));
    assert(t && !pthread_mutex_init(&t->lock, NULL));
    cliaddr_t a = { .v4 = { .sin_family=AF_INET, .sin_port=htons(1000) } }, b=a, out;
    b.v4.sin_port=htons(1001);
    uint32_t x,y,local;
    assert(normal_register(t,&a,42,0,&x));
    assert(normal_register(t,&b,42,0,&y) && x!=y);
    assert(normal_lookup(t,x,0,&out,&local) && local==42 && cliaddr_eq(&a,&out));
    assert(!normal_lookup(t,x,1,&out,&local));
    assert(normal_lookup(t,y,1,&out,&local) && cliaddr_eq(&b,&out));
    assert(normal_peer_addr(t,1,&out) && cliaddr_eq(&b,&out));
    for (unsigned i=2;i<NORMAL_SLOTS;i++) assert(normal_register(t,&a,100+i,0,&y));
    assert(!normal_register(t,&a,999999,0,&y));
    assert(normal_lookup(t,x,0,&out,&local));
    t->slots[x & (NORMAL_SLOTS-1)].seen=normal_now()-NORMAL_IDLE;
    assert(!normal_lookup(t,x,-1,&out,&local));
    assert(normal_register(t,&a,999999,0,&y) && x!=y);
    assert(!normal_lookup(t,x,-1,&out,&local));
    assert(normal_lookup(t,y,-1,&out,&local) && local==999999);
    pthread_mutex_destroy(&t->lock);
    free(t);
    puts("PASS normal session capacity, collision isolation, recipient binding, expiry and stale generation");
}
