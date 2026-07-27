#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CURL CURL;
typedef struct curl_mime curl_mime;
typedef struct curl_mimepart curl_mimepart;
typedef long long curl_off_t;

struct curl_slist {
    char* data;
    struct curl_slist* next;
};

typedef enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_PARTIAL_FILE = 18,
    CURLE_HTTP_RETURNED_ERROR = 22,
    CURLE_RECV_ERROR = 56,
} CURLcode;

#define CURL_GLOBAL_ALL 0x03L
#define CURL_GLOBAL_DEFAULT CURL_GLOBAL_ALL

#define CURLAUTH_ANY (~0L)

#define CURLOPT_URL 10002
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_TIMEOUT 13
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_POST 47
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_SSL_VERIFYHOST 81
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_HTTPAUTH 107
#define CURLOPT_USERPWD 10005
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_NOBODY 44
#define CURLOPT_MIMEPOST 10269
#define CURLOPT_NOPROGRESS 43
#define CURLOPT_XFERINFOFUNCTION 20219
#define CURLOPT_XFERINFODATA 10220
#define CURLOPT_CUSTOMREQUEST 10036
#define CURLOPT_COOKIEJAR 10082
#define CURLOPT_COOKIEFILE 10031

#define CURLINFO_RESPONSE_CODE 0x200002
#define CURLINFO_COOKIELIST 0x40001c

static inline CURLcode curl_global_init(long flags)
{
    (void)flags;
    return CURLE_FAILED_INIT;
}

static inline void curl_global_cleanup(void) {}

static inline CURL* curl_easy_init(void)
{
    return (CURL*)0;
}

static inline void curl_easy_cleanup(CURL* curl)
{
    (void)curl;
}

static inline CURLcode curl_easy_setopt(CURL* curl, int option, ...)
{
    (void)curl;
    (void)option;
    return CURLE_FAILED_INIT;
}

static inline CURLcode curl_easy_perform(CURL* curl)
{
    (void)curl;
    return CURLE_FAILED_INIT;
}

static inline CURLcode curl_easy_getinfo(CURL* curl, int info, ...)
{
    (void)curl;
    (void)info;
    return CURLE_FAILED_INIT;
}

static inline const char* curl_easy_strerror(CURLcode error)
{
    (void)error;
    return "libcurl unavailable in this Virtua build";
}

static inline struct curl_slist* curl_slist_append(struct curl_slist* list, const char* data)
{
    (void)data;
    return list;
}

static inline void curl_slist_free_all(struct curl_slist* list)
{
    (void)list;
}

static inline curl_mime* curl_mime_init(CURL* curl)
{
    (void)curl;
    return (curl_mime*)0;
}

static inline void curl_mime_free(curl_mime* mime)
{
    (void)mime;
}

static inline curl_mimepart* curl_mime_addpart(curl_mime* mime)
{
    (void)mime;
    return (curl_mimepart*)0;
}

static inline CURLcode curl_mime_name(curl_mimepart* part, const char* name)
{
    (void)part;
    (void)name;
    return CURLE_FAILED_INIT;
}

static inline CURLcode curl_mime_filename(curl_mimepart* part, const char* filename)
{
    (void)part;
    (void)filename;
    return CURLE_FAILED_INIT;
}

static inline CURLcode curl_mime_filedata(curl_mimepart* part, const char* filename)
{
    (void)part;
    (void)filename;
    return CURLE_FAILED_INIT;
}

static inline CURLcode curl_mime_type(curl_mimepart* part, const char* mimetype)
{
    (void)part;
    (void)mimetype;
    return CURLE_FAILED_INIT;
}

#ifdef __cplusplus
}
#endif
