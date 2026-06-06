#ifndef AWG_LOG_H
#define AWG_LOG_H

enum { LOG_NONE = 0, LOG_ERROR = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

extern int g_log_level;

void log_msg(const char *prefix, const char *msg);
void log_msg2(const char *prefix, const char *a, const char *b);
void log_msg3(const char *prefix, const char *a, const char *b, const char *c);
void log_msgn(const char *prefix, const char **parts, int count);

#define log_info(...)  do { if (g_log_level >= LOG_INFO)  log_msg("INFO: ",  __VA_ARGS__); } while(0)
#define log_error(...) do { if (g_log_level >= LOG_ERROR) log_msg("ERROR: ", __VA_ARGS__); } while(0)
#define log_debug(...) do { if (g_log_level >= LOG_DEBUG) log_msg("DEBUG: ", __VA_ARGS__); } while(0)

#define log_info2(a,b)  do { if (g_log_level >= LOG_INFO)  log_msg2("INFO: ",  a, b); } while(0)
#define log_error2(a,b) do { if (g_log_level >= LOG_ERROR) log_msg2("ERROR: ", a, b); } while(0)
#define log_debug2(a,b) do { if (g_log_level >= LOG_DEBUG) log_msg2("DEBUG: ", a, b); } while(0)

#define log_info3(a,b,c)  do { if (g_log_level >= LOG_INFO)  log_msg3("INFO: ",  a, b, c); } while(0)
#define log_error3(a,b,c) do { if (g_log_level >= LOG_ERROR) log_msg3("ERROR: ", a, b, c); } while(0)
#define log_debug3(a,b,c) do { if (g_log_level >= LOG_DEBUG) log_msg3("DEBUG: ", a, b, c); } while(0)

#define log_infon(p,n)  do { if (g_log_level >= LOG_INFO)  log_msgn("INFO: ",  p, n); } while(0)
#define log_debugn(p,n) do { if (g_log_level >= LOG_DEBUG) log_msgn("DEBUG: ", p, n); } while(0)

/* itoa into caller buffer, returns pointer to start of number within buf */
char *u32_to_str(char *buf, unsigned int v);

#endif
