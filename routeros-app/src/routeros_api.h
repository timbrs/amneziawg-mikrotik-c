#ifndef ROUTEROS_API_H
#define ROUTEROS_API_H

#include <stddef.h>

#define ROS_API_MAX_WORDS 64
#define ROS_API_MAX_WORD 1024
#define ROS_API_MAX_REPLIES 128

typedef struct {
    int fd;
    char last_error[256];
} ros_api_t;

typedef struct {
    int count;
    char words[ROS_API_MAX_WORDS][ROS_API_MAX_WORD];
} ros_sentence_t;

typedef struct {
    int count;
    ros_sentence_t sentences[ROS_API_MAX_REPLIES];
} ros_reply_t;

int ros_api_connect(ros_api_t *api, const char *host, int port, const char *user, const char *password);
void ros_api_close(ros_api_t *api);
int ros_api_command(ros_api_t *api, const char *const *words, int word_count, ros_reply_t *reply);
const char *ros_reply_find(const ros_sentence_t *sentence, const char *key);
int ros_reply_has_trap(const ros_reply_t *reply, char *message, size_t message_len);
const ros_sentence_t *ros_reply_first_re(const ros_reply_t *reply);

#endif
