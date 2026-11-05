/* Run the real c2s loop against local datagram sockets. Pause a batch at
 * sendmmsg, then let s2c attempt to retire the socket that batch selected.
 * Linker wrappers control only the ordering of the two threads. */
#include "proxy.c"
#include "test.h"
#undef NDEBUG
#include <assert.h>

static proxy_t *proxy;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int selected_fd;
static int send_waiting, retire_attempted, send_finished;
static _Thread_local int retiring;
static const uint8_t transport[WG_TRANSPORT_MIN] = { WG_TRANSPORT_DATA };

int __real_sendmmsg(int fd, struct mmsghdr *msgs, unsigned int count, int flags);
int __real_close(int fd);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *lock);

int __wrap_sendmmsg(int fd, struct mmsghdr *msgs, unsigned int count, int flags) {
    if (fd != selected_fd)
        return __real_sendmmsg(fd, msgs, count, flags);

    pthread_mutex_lock(&mu);
    send_waiting = 1;
    pthread_cond_broadcast(&changed);
    while (!retire_attempted)
        pthread_cond_wait(&changed, &mu);

    int sent = __real_sendmmsg(fd, msgs, count, flags);
    int saved_errno = errno;
    atomic_store(&proxy->stopped, 1);
    send_finished = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mu);
    errno = saved_errno;
    return sent;
}

int __wrap_close(int fd) {
    int result = __real_close(fd);
    if (retiring && fd == selected_fd) {
        /* Hold the retire thread here until sendmmsg has used the selected fd,
         * so reconnect cannot open any other socket during the assertion. */
        pthread_mutex_lock(&mu);
        retire_attempted = 1;
        pthread_cond_broadcast(&changed);
        while (!send_finished)
            pthread_cond_wait(&changed, &mu);
        pthread_mutex_unlock(&mu);
    }
    return result;
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
    if (!retiring) return __real_pthread_rwlock_wrlock(lock);
    int result = pthread_rwlock_trywrlock(lock);
    if (result != EBUSY) return result;

    /* The writer found a live read-side operation. Let that operation
     * finish, then wait for the write lock as the production code would. */
    pthread_mutex_lock(&mu);
    retire_attempted = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mu);
    return __real_pthread_rwlock_wrlock(lock);
}

static void *replace_remote(void *alternate) {
    retiring = 1;
    if (alternate)
        he_finish(proxy, 1, 0);
    else
        assert(do_reconnect(proxy) >= 0);
    return NULL;
}

static int in_flight_send_survives_replacement(proxy_t *fixture, int alternate) {
    proxy = fixture;
    awg_config_t cfg = {
        .mode = AWG_MODE_NORMAL, .no_gso = 1, .cpu_c2s = -1,
        .h1 = { 1, 1 }, .h2 = { 2, 2 }, .h3 = { 3, 3 }, .h4 = { 4, 4 }
    };
    config_compute(&cfg);
    assert(proxy_init(proxy, &cfg, "127.0.0.1:51820", "127.0.0.1:9", -1) == 0);

    int client[2], remote[2], other[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, client) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, remote) == 0);
    proxy->listen_fd = client[0];
    selected_fd = remote[0];
    atomic_store(&proxy->remote_fd, selected_fd);
    if (alternate) {
        assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, other) == 0);
        proxy->remote_alt.sa.ss_family = AF_INET;
        atomic_store(&proxy->remote_fd2, other[0]);
    }
    send_waiting = retire_attempted = send_finished = 0;

    pthread_t sender, writer;
    assert(pthread_create(&sender, NULL, c2s_thread_normal, proxy) == 0);
    assert(send(client[1], transport, sizeof(transport), 0) == (ssize_t)sizeof(transport));
    pthread_mutex_lock(&mu);
    while (!send_waiting)
        pthread_cond_wait(&changed, &mu);
    pthread_mutex_unlock(&mu);
    assert(pthread_create(&writer, NULL, replace_remote, alternate ? proxy : NULL) == 0);
    assert(pthread_join(sender, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);

    uint8_t packet[sizeof(transport)];
    ssize_t n = recv(remote[1], packet, sizeof(packet), MSG_DONTWAIT);
    if (n < 0)
        fprintf(stderr, "  in-flight batch never reached the original socket\n");
    close(atomic_load(&proxy->remote_fd));
    if (alternate) close(other[1]);
    close(proxy->he_evfd);
    close(remote[1]);
    close(client[0]);
    close(client[1]);
    pthread_mutex_destroy(&proxy->remote_mu);
    return n == (ssize_t)sizeof(packet) && memcmp(packet, transport, sizeof(packet)) == 0;
}

static void test_send_during_reconnect(void) {
    static proxy_t fixture;
    ASSERT(in_flight_send_survives_replacement(&fixture, 0));
}

static void test_send_during_family_switch(void) {
    static proxy_t fixture;
    ASSERT(in_flight_send_survives_replacement(&fixture, 1));
}

int main(void) {
    alarm(10); /* bound a test failure, independent of thread ordering */
    g_log_level = LOG_ERROR;
    RUN_TEST(send_during_reconnect);
    RUN_TEST(send_during_family_switch);
    TEST_MAIN_END();
}
