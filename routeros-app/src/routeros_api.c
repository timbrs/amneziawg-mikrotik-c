#include "routeros_api.h"

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void api_err(ros_api_t *api, const char *msg) {
    snprintf(api->last_error, sizeof(api->last_error), "%s", msg);
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_len(int fd, size_t len) {
    unsigned char b[5];
    int n;
    if (len < 0x80) {
        b[0] = (unsigned char)len;
        n = 1;
    } else if (len < 0x4000) {
        b[0] = (unsigned char)((len >> 8) | 0x80);
        b[1] = (unsigned char)len;
        n = 2;
    } else if (len < 0x200000) {
        b[0] = (unsigned char)((len >> 16) | 0xC0);
        b[1] = (unsigned char)(len >> 8);
        b[2] = (unsigned char)len;
        n = 3;
    } else if (len < 0x10000000) {
        b[0] = (unsigned char)((len >> 24) | 0xE0);
        b[1] = (unsigned char)(len >> 16);
        b[2] = (unsigned char)(len >> 8);
        b[3] = (unsigned char)len;
        n = 4;
    } else {
        b[0] = 0xF0;
        b[1] = (unsigned char)(len >> 24);
        b[2] = (unsigned char)(len >> 16);
        b[3] = (unsigned char)(len >> 8);
        b[4] = (unsigned char)len;
        n = 5;
    }
    return write_all(fd, b, (size_t)n);
}

static int read_len(int fd, size_t *len) {
    unsigned char c;
    if (read_all(fd, &c, 1) < 0) return -1;
    if ((c & 0x80) == 0) {
        *len = c;
    } else if ((c & 0xC0) == 0x80) {
        unsigned char b[1];
        if (read_all(fd, b, 1) < 0) return -1;
        *len = (((size_t)c & ~0xC0u) << 8) | b[0];
    } else if ((c & 0xE0) == 0xC0) {
        unsigned char b[2];
        if (read_all(fd, b, 2) < 0) return -1;
        *len = (((size_t)c & ~0xE0u) << 16) | ((size_t)b[0] << 8) | b[1];
    } else if ((c & 0xF0) == 0xE0) {
        unsigned char b[3];
        if (read_all(fd, b, 3) < 0) return -1;
        *len = (((size_t)c & ~0xF0u) << 24) | ((size_t)b[0] << 16) | ((size_t)b[1] << 8) | b[2];
    } else {
        unsigned char b[4];
        if (read_all(fd, b, 4) < 0) return -1;
        *len = ((size_t)b[0] << 24) | ((size_t)b[1] << 16) | ((size_t)b[2] << 8) | b[3];
    }
    return 0;
}

static int write_word(int fd, const char *word) {
    size_t len = strlen(word);
    if (write_len(fd, len) < 0) return -1;
    return write_all(fd, word, len);
}

static int write_sentence(int fd, const char *const *words, int word_count) {
    for (int i = 0; i < word_count; i++) {
        if (write_word(fd, words[i]) < 0) return -1;
    }
    return write_len(fd, 0);
}

static int read_sentence(int fd, ros_sentence_t *s) {
    memset(s, 0, sizeof(*s));
    for (;;) {
        size_t len;
        if (read_len(fd, &len) < 0) return -1;
        if (len == 0) return 0;
        if (s->count >= ROS_API_MAX_WORDS || len >= ROS_API_MAX_WORD) return -1;
        if (read_all(fd, s->words[s->count], len) < 0) return -1;
        s->words[s->count][len] = '\0';
        s->count++;
    }
}

static int read_reply(int fd, ros_reply_t *reply) {
    memset(reply, 0, sizeof(*reply));
    for (;;) {
        if (reply->count >= ROS_API_MAX_REPLIES) return -1;
        ros_sentence_t *s = &reply->sentences[reply->count++];
        if (read_sentence(fd, s) < 0) return -1;
        if (s->count > 0 && strcmp(s->words[0], "!done") == 0) return 0;
    }
}

int ros_api_command(ros_api_t *api, const char *const *words, int word_count, ros_reply_t *reply) {
    if (write_sentence(api->fd, words, word_count) < 0) {
        api_err(api, "RouterOS API write failed");
        return -1;
    }
    if (read_reply(api->fd, reply) < 0) {
        api_err(api, "RouterOS API read failed");
        return -1;
    }
    char trap[256];
    if (ros_reply_has_trap(reply, trap, sizeof(trap))) {
        snprintf(api->last_error, sizeof(api->last_error), "RouterOS API trap: %s", trap);
        return -1;
    }
    return 0;
}

static int tcp_connect(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int ros_api_connect(ros_api_t *api, const char *host, int port, const char *user, const char *password) {
    memset(api, 0, sizeof(*api));
    api->fd = -1;

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        api_err(api, "cannot connect to RouterOS API");
        return -1;
    }
    api->fd = fd;

    char uword[ROS_API_MAX_WORD];
    char pword[ROS_API_MAX_WORD];
    snprintf(uword, sizeof(uword), "=name=%s", user);
    snprintf(pword, sizeof(pword), "=password=%s", password);
    const char *login[] = { "/login", uword, pword };
    static ros_reply_t reply;
    if (ros_api_command(api, login, 3, &reply) < 0) {
        ros_api_close(api);
        return -1;
    }
    return 0;
}

void ros_api_close(ros_api_t *api) {
    if (api->fd >= 0) close(api->fd);
    api->fd = -1;
}

const char *ros_reply_find(const ros_sentence_t *sentence, const char *key) {
    size_t key_len = strlen(key);
    for (int i = 0; i < sentence->count; i++) {
        const char *w = sentence->words[i];
        if (w[0] == '=' && strncmp(w + 1, key, key_len) == 0 && w[1 + key_len] == '=') {
            return w + 1 + key_len + 1;
        }
    }
    return NULL;
}

int ros_reply_has_trap(const ros_reply_t *reply, char *message, size_t message_len) {
    for (int i = 0; i < reply->count; i++) {
        const ros_sentence_t *s = &reply->sentences[i];
        if (s->count > 0 && strcmp(s->words[0], "!trap") == 0) {
            const char *m = ros_reply_find(s, "message");
            snprintf(message, message_len, "%s", m ? m : "unknown trap");
            return 1;
        }
    }
    return 0;
}

const ros_sentence_t *ros_reply_first_re(const ros_reply_t *reply) {
    for (int i = 0; i < reply->count; i++) {
        if (reply->sentences[i].count > 0 && strcmp(reply->sentences[i].words[0], "!re") == 0)
            return &reply->sentences[i];
    }
    return NULL;
}
