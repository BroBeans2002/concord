#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord.h"
#include "discord-internal.h"

static struct discord_request *
_discord_request_init(void)
{
    return calloc(1, sizeof(struct discord_request));
}

static void
_discord_request_cleanup(struct discord_request *req)
{
    discord_attachments_cleanup(&req->attachments);
    if (req->body.start) free(req->body.start);
    free(req);
}

static int
_discord_on_rest_perform(struct io_poller *io, CURLM *mhandle, void *p_rest)
{
    (void)io;
    (void)mhandle;
    return discord_rest_perform(p_rest);
}

static void
_discord_on_curl_setopt(struct ua_conn *conn, void *p_token)
{
    struct ccord_szbuf *token = p_token;
    char auth[128];
    int len;

    len = snprintf(auth, sizeof(auth), "Bot %.*s", (int)token->size,
                   token->start);
    ASSERT_NOT_OOB(len, sizeof(auth));

    ua_conn_add_header(conn, "Authorization", auth);

#ifdef CCORD_DEBUG_HTTP
    curl_easy_setopt(ua_conn_get_easy_handle(conn), CURLOPT_VERBOSE, 1L);
#endif
}

void
discord_requestor_init(struct discord_requestor *rqtor,
                       struct logconf *conf,
                       struct ccord_szbuf_readonly *token)
{
    struct discord_rest *rest =
        CONTAINEROF(rqtor, struct discord_rest, requestor);

    logconf_branch(&rqtor->conf, conf, "DISCORD_REQUEST");

    rqtor->ua = ua_init(&(struct ua_attr){ .conf = conf });
    ua_set_url(rqtor->ua, DISCORD_API_BASE_URL);
    ua_set_opt(rqtor->ua, token, &_discord_on_curl_setopt);

    /* queues are malloc'd to guarantee a client cloned by
     * discord_clone() will share the same queue with the original */
    rqtor->queues = malloc(sizeof *rqtor->queues);
    QUEUE_INIT(&rqtor->queues->recycling);
    QUEUE_INIT(&rqtor->queues->pending);
    QUEUE_INIT(&rqtor->queues->finished);

    rqtor->mhandle = curl_multi_init();
    io_poller_curlm_add(rest->io_poller, rqtor->mhandle,
                        &_discord_on_rest_perform, rest);

    rqtor->retry_limit = 3; /* FIXME: shouldn't be a hard limit */

    discord_ratelimiter_init(&rqtor->ratelimiter, &rqtor->conf);
}

void
discord_requestor_cleanup(struct discord_requestor *rqtor)
{
    struct discord_rest *rest =
        CONTAINEROF(rqtor, struct discord_rest, requestor);
    QUEUE *const req_queues[] = { &rqtor->queues->recycling,
                                  &rqtor->queues->pending,
                                  &rqtor->queues->finished };

    /* cleanup ratelimiting handle */
    discord_ratelimiter_cleanup(&rqtor->ratelimiter);

    /* cleanup request structs */
    for (size_t i = 0; i < sizeof(req_queues) / sizeof *req_queues; ++i) {
        QUEUE(struct discord_request) queue, *qelem;
        struct discord_request *req;

        QUEUE_MOVE(req_queues[i], &queue);
        while (!QUEUE_EMPTY(&queue)) {
            qelem = QUEUE_HEAD(&queue);
            QUEUE_REMOVE(qelem);

            req = QUEUE_DATA(qelem, struct discord_request, entry);
            _discord_request_cleanup(req);
        }
    }
    free(rqtor->queues);

    /* cleanup curl's multi handle */
    io_poller_curlm_del(rest->io_poller, rqtor->mhandle);
    curl_multi_cleanup(rqtor->mhandle);
    /* cleanup User-Agent handle */
    ua_cleanup(rqtor->ua);
}

static void
_discord_request_to_multipart(curl_mime *mime, void *p_req)
{
    struct discord_request *req = p_req;
    curl_mimepart *part;
    char name[64];

    /* json part */
    if (req->body.start && req->body.size) {
        part = curl_mime_addpart(mime);
        curl_mime_data(part, req->body.start, req->body.size);
        curl_mime_type(part, "application/json");
        curl_mime_name(part, "payload_json");
    }

    /* attachment part */
    for (int i = 0; i < req->attachments.size; ++i) {
        int len = snprintf(name, sizeof(name), "files[%d]", i);
        ASSERT_NOT_OOB(len, sizeof(name));

        if (req->attachments.array[i].content) {
            part = curl_mime_addpart(mime);
            curl_mime_data(part, req->attachments.array[i].content,
                           req->attachments.array[i].size
                               ? req->attachments.array[i].size
                               : CURL_ZERO_TERMINATED);
            curl_mime_filename(part, !req->attachments.array[i].filename
                                         ? "a.out"
                                         : req->attachments.array[i].filename);
            curl_mime_type(part, !req->attachments.array[i].content_type
                                     ? "application/octet-stream"
                                     : req->attachments.array[i].content_type);
            curl_mime_name(part, name);
        }
        else if (req->attachments.array[i].filename) {
            CURLcode code;

            /* fetch local file by the filename */
            part = curl_mime_addpart(mime);
            code =
                curl_mime_filedata(part, req->attachments.array[i].filename);
            if (code != CURLE_OK) {
                char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "%s (file: %s)",
                         curl_easy_strerror(code),
                         req->attachments.array[i].filename);
                perror(errbuf);
            }
            curl_mime_type(part, !req->attachments.array[i].content_type
                                     ? "application/octet-stream"
                                     : req->attachments.array[i].content_type);
            curl_mime_name(part, name);
        }
    }
}

static void
_discord_request_info_extract(struct discord_requestor *rqtor,
                              struct discord_request *req,
                              struct ua_info *info)
{
    ua_info_extract(req->conn, info);

    if (info->code != CCORD_HTTP_CODE) { /* CCORD_OK or internal error */
        req->retry = false;
    }
    else {
        switch (info->httpcode) {
        case HTTP_FORBIDDEN:
        case HTTP_NOT_FOUND:
        case HTTP_BAD_REQUEST:
            info->code = CCORD_DISCORD_JSON_CODE;
            req->retry = false;
            break;
        case HTTP_UNAUTHORIZED:
            logconf_fatal(
                &rqtor->conf,
                "UNAUTHORIZED: Please provide a valid authentication token");
            info->code = CCORD_DISCORD_BAD_AUTH;
            req->retry = false;
            break;
        case HTTP_METHOD_NOT_ALLOWED:
            logconf_fatal(
                &rqtor->conf,
                "METHOD_NOT_ALLOWED: The server couldn't recognize the "
                "received HTTP method");
            req->retry = false;
            break;
        case HTTP_TOO_MANY_REQUESTS: {
            struct ua_szbuf_readonly body = ua_info_get_body(info);
            struct jsmnftok message = { 0 };
            double retry_after = 1.0;
            bool is_global = false;
            jsmn_parser parser;
            jsmntok_t tokens[16];

            jsmn_init(&parser);
            if (0 < jsmn_parse(&parser, body.start, body.size, tokens,
                               sizeof(tokens) / sizeof *tokens))
            {
                jsmnf_loader loader;
                jsmnf_pair pairs[16];

                jsmnf_init(&loader);
                if (0 < jsmnf_load(&loader, body.start, tokens, parser.toknext,
                                   pairs, sizeof(pairs) / sizeof *pairs))
                {
                    jsmnf_pair *f;

                    if ((f = jsmnf_find(pairs, body.start, "global", 6)))
                        is_global = ('t' == body.start[f->v.pos]);
                    if ((f = jsmnf_find(pairs, body.start, "message", 7)))
                        message = f->v;
                    if ((f = jsmnf_find(pairs, body.start, "retry_after", 11)))
                        retry_after = strtod(body.start + f->v.pos, NULL);
                }
            }

            req->wait_ms = (int64_t)(1000 * retry_after);
            if (req->wait_ms < 0) req->wait_ms = 0;

            logconf_warn(&rqtor->conf,
                         "429 %sRATELIMITING (wait: %" PRId64 " ms) : %.*s",
                         is_global ? "GLOBAL " : "", req->wait_ms, message.len,
                         body.start + message.pos);

            req->retry = true;
            break;
        }
        default:
            req->retry = (info->httpcode >= 500); /* retry if Server Error */
            break;
        }
    }
}

/**
 * @brief If request can be retried then it will be moved back to its
 *      bucket's queue
 * @note this **MUST** be called only after discord_request_info_extract()
 *
 * @param rqtor the requestor handle initialized with discord_requestor_init()
 * @param req the request to be checked for retry
 * @return `true` if request has been enqueued for retry
 */
static bool
_discord_request_retry(struct discord_requestor *rqtor,
                       struct discord_request *req)
{
    if (!req->retry || req->retry_attempt++ >= rqtor->retry_limit)
        return false;

    req->b->performing_req = NULL;
    ua_conn_reset(req->conn);

    /* FIXME: wait_ms > 0 should be dealt with aswell */
    if (req->wait_ms <= 0) discord_bucket_add_request(req->b, req, true);

    return true;
}

void
discord_request_cancel(struct discord_requestor *rqtor,
                       struct discord_request *req)
{
    struct discord_refcounter *rc = &CLIENT(rqtor, rest.requestor)->refcounter;

    if (req->conn) ua_conn_stop(req->conn);

    if (req->dispatch.keep)
        discord_refcounter_decr(rc, (void *)req->dispatch.keep);
    if (req->dispatch.data) discord_refcounter_decr(rc, req->dispatch.data);

    req->b->performing_req = NULL;
    req->body.size = 0;
    req->method = 0;
    *req->endpoint = '\0';
    *req->key = '\0';
    req->conn = NULL;
    req->retry_attempt = 0;
    discord_attachments_cleanup(&req->attachments);
    memset(req, 0, sizeof(struct discord_attributes));

    QUEUE_REMOVE(&req->entry);
    QUEUE_INIT(&req->entry);
    QUEUE_INSERT_TAIL(&rqtor->queues->recycling, &req->entry);
}

static CCORDcode
_discord_request_dispatch_response(struct discord_requestor *rqtor,
                                   struct discord_request *req)
{
    struct discord *client = CLIENT(rqtor, rest.requestor);
    struct discord_response resp = { .data = req->dispatch.data,
                                     .keep = req->dispatch.keep,
                                     .code = req->code };

    if (req->code != CCORD_OK) {
        req->dispatch.fail(client, &resp);
    }
    else if (req->dispatch.done.typed) {
        if (!req->dispatch.has_type) {
            req->dispatch.done.typeless(client, &resp);
        }
        else {
            req->dispatch.done.typed(client, &resp, req->response.data);
            discord_refcounter_decr(&client->refcounter, req->response.data);
        }
    }

    /* enqueue request for retry or recycle */
    if (!_discord_request_retry(rqtor, req))
        discord_request_cancel(rqtor, req);

    return resp.code;
}

void
discord_requestor_dispatch_responses(struct discord_requestor *rqtor)
{
    struct discord_rest *rest =
        CONTAINEROF(rqtor, struct discord_rest, requestor);

    if (0 == pthread_mutex_trylock(rest->g_lock)) {
        if (!QUEUE_EMPTY(&rqtor->queues->finished)) {
            QUEUE(struct discord_request) queue, *qelem;
            struct discord_request *req;

            QUEUE_MOVE(&rqtor->queues->finished, &queue);
            do {
                qelem = QUEUE_HEAD(&queue);
                req = QUEUE_DATA(qelem, struct discord_request, entry);
                _discord_request_dispatch_response(rqtor, req);
            } while (!QUEUE_EMPTY(&queue));

            io_poller_wakeup(rest->io_poller);
        }
        pthread_mutex_unlock(rest->g_lock);
    }
}

/* parse request response and prepare callback that should be triggered
 * at _discord_rest_run_request_callback() */
CCORDcode
discord_requestor_info_read(struct discord_requestor *rqtor)
{
    CCORDcode code;
    int alive = 0;

    if (CURLM_OK != curl_multi_socket_all(rqtor->mhandle, &alive))
        return CCORD_CURLM_INTERNAL;

    while (1) {
        int msgq = 0;
        struct CURLMsg *msg = curl_multi_info_read(rqtor->mhandle, &msgq);

        if (!msg) break;

        if (CURLMSG_DONE == msg->msg) {
            const CURLcode ecode = msg->data.result;
            struct discord_request *req;

            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req);
            curl_multi_remove_handle(rqtor->mhandle, msg->easy_handle);

            switch (ecode) {
            case CURLE_OK: {
                struct ua_szbuf_readonly body;
                struct ua_info info;

                _discord_request_info_extract(rqtor, req, &info);
                body = ua_info_get_body(&info);

                if (info.code != CCORD_OK) {
                    logconf_error(&rqtor->conf, "%.*s", (int)body.size,
                                  body.start);
                }
                else if (req->dispatch.has_type
                         && req->dispatch.sync != DISCORD_SYNC_FLAG) {
                    if (req->dispatch.sync) {
                        req->response.data = req->dispatch.sync;
                    }
                    else {
                        req->response.data = calloc(1, req->response.size);
                        discord_refcounter_add_internal(
                            &CLIENT(rqtor, rest.requestor)->refcounter,
                            req->response.data, req->response.cleanup, true);
                    }

                    /* initialize ret */
                    if (req->response.init)
                        req->response.init(req->response.data);
                    /* populate ret */
                    if (req->response.from_json)
                        req->response.from_json(body.start, body.size,
                                                req->response.data);
                }

                discord_ratelimiter_build(&rqtor->ratelimiter, req->b,
                                          req->key, &info);
                ua_info_cleanup(&info);
            } break;
            case CURLE_READ_ERROR:
                logconf_warn(&rqtor->conf, "%s (CURLE code: %d)",
                             curl_easy_strerror(ecode), ecode);

                req->retry = true;
                req->code = CCORD_CURLE_INTERNAL;

                break;
            default:
                logconf_error(&rqtor->conf, "%s (CURLE code: %d)",
                              curl_easy_strerror(ecode), ecode);

                req->retry = false;
                req->code = CCORD_CURLE_INTERNAL;

                break;
            }

            code = req->code;

            if (req->dispatch.sync)
                pthread_cond_signal(req->cond);
            else
                QUEUE_INSERT_TAIL(&rqtor->queues->finished, &req->entry);
        }
    }

    return code;
}

static void
_discord_request_try_begin(struct discord_ratelimiter *rl,
                           struct discord_bucket *b)
{
    /* skip if bucket is already busy performing */
    if (b->performing_req) return;

    if (!b->remaining) {
        discord_bucket_try_timeout(rl, b);
    }
    else if (!QUEUE_EMPTY(&b->pending_queue)) {
        struct discord_requestor *rqtor =
            CONTAINEROF(rl, struct discord_requestor, ratelimiter);

        struct discord_request *req = discord_bucket_remove_request(b);
        CURL *ehandle;

        b->performing_req = req;
        req->conn = ua_conn_start(rqtor->ua);
        ehandle = ua_conn_get_easy_handle(req->conn);

        if (HTTP_MIMEPOST == req->method) {
            ua_conn_add_header(req->conn, "Content-Type",
                               "multipart/form-data");
            ua_conn_set_mime(req->conn, req, &_discord_request_to_multipart);
        }
        else {
            ua_conn_add_header(req->conn, "Content-Type", "application/json");
        }

        ua_conn_setup(req->conn, &(struct ua_conn_attr){
                                     .method = req->method,
                                     .body = req->body.start,
                                     .body_size = req->body.size,
                                     .endpoint = req->endpoint,
                                     .base_url = NULL,
                                 });

        /* link 'req' to 'ehandle' for easy retrieval */
        curl_easy_setopt(ehandle, CURLOPT_PRIVATE, req);

        /* initiate libcurl transfer */
        curl_multi_add_handle(rqtor->mhandle, ehandle);
    }
}

CCORDcode
discord_requestor_start_pending(struct discord_requestor *rqtor)
{
    QUEUE(struct discord_request) queue, *qelem;
    struct discord_request *req;
    struct discord_bucket *b;

    /* match pending requests to their buckets */
    QUEUE_MOVE(&rqtor->queues->pending, &queue);
    while (!QUEUE_EMPTY(&queue)) {
        qelem = QUEUE_HEAD(&queue);
        QUEUE_REMOVE(qelem);

        req = QUEUE_DATA(qelem, struct discord_request, entry);
        b = discord_bucket_get(&rqtor->ratelimiter, req->key);
        discord_bucket_add_request(b, req, req->dispatch.high_p);
    }

    /* TODO: replace foreach with a mechanism that loops only busy buckets */
    discord_ratelimiter_foreach_bucket(&rqtor->ratelimiter,
                                       &_discord_request_try_begin);

    /* FIXME: redundant return value (constant) */
    return CCORD_OK;
}

/* Only fields required at _discord_request_to_multipart() are duplicated */
static void
_discord_attachments_dup(struct discord_attachments *dest,
                         struct discord_attachments *src)
{
    __carray_init(dest, (size_t)src->size, struct discord_attachment, , );
    for (int i = 0; i < src->size; ++i) {
        carray_insert(dest, i, src->array[i]);
        if (src->array[i].content) {
            dest->array[i].size = src->array[i].size
                                      ? src->array[i].size
                                      : strlen(src->array[i].content) + 1;

            dest->array[i].content = malloc(dest->array[i].size);
            memcpy(dest->array[i].content, src->array[i].content,
                   dest->array[i].size);
        }
        if (src->array[i].filename)
            dest->array[i].filename = strdup(src->array[i].filename);
        if (src->array[i].content_type)
            dest->array[i].content_type = strdup(src->array[i].content_type);
    }
}

static struct discord_request *
_discord_request_get(struct discord_requestor *rqtor)
{
    struct discord_request *req;

    if (QUEUE_EMPTY(&rqtor->queues->recycling)) { /* new request struct */
        req = _discord_request_init();
    }
    else { /* fetch a request struct from queues->recycling */
        QUEUE(struct discord_request) *qelem =
            QUEUE_HEAD(&rqtor->queues->recycling);

        QUEUE_REMOVE(qelem);
        req = QUEUE_DATA(qelem, struct discord_request, entry);
    }
    QUEUE_INIT(&req->entry);

    return req;
}

CCORDcode
discord_request_begin(struct discord_requestor *rqtor,
                      struct discord_attributes *attr,
                      struct ccord_szbuf *body,
                      enum http_method method,
                      char endpoint[DISCORD_ENDPT_LEN],
                      char key[DISCORD_ROUTE_LEN])
{
    struct discord_rest *rest =
        CONTAINEROF(rqtor, struct discord_rest, requestor);
    struct discord *client = CLIENT(rest, rest);
    struct discord_request *req = _discord_request_get(rqtor);
    CCORDcode code;

    pthread_mutex_lock(rest->g_lock);

    req->method = method;
    memcpy(req, attr, sizeof *attr);

    if (attr->attachments.size)
        _discord_attachments_dup(&req->attachments, &attr->attachments);

    if (body) {
        /* copy request body */
        if (body->size > req->body.realsize) {
            /* needs to increase buffer size */
            void *tmp = realloc(req->body.start, body->size);
            ASSERT_S(tmp != NULL, "Out of memory");

            req->body.start = tmp;
            req->body.realsize = body->size;
        }
        memcpy(req->body.start, body->start, body->size);
        req->body.size = body->size;
    }

    /* copy endpoint over to req */
    memcpy(req->endpoint, endpoint, sizeof(req->endpoint));
    /* copy bucket's key */
    memcpy(req->key, key, sizeof(req->key));

    req->cond = NULL;

    if (attr->dispatch.keep) {
        code = discord_refcounter_incr(&client->refcounter,
                                       (void *)attr->dispatch.keep);

        ASSERT_S(code == CCORD_OK,
                 "'.keep' data must be a Concord callback parameter");
    }
    if (attr->dispatch.data
        && CCORD_UNAVAILABLE
               == discord_refcounter_incr(&client->refcounter,
                                          attr->dispatch.data))
    {
        discord_refcounter_add_client(&client->refcounter, attr->dispatch.data,
                                      attr->dispatch.cleanup, false);
    }

    /* request will be assigned to its bucket at the REST thread  */
    QUEUE_INSERT_TAIL(&rqtor->queues->pending, &req->entry);

    io_poller_wakeup(rest->io_poller);
    if (!req->dispatch.sync) {
        code = CCORD_OK;
    }
    else {
        req->cond = &(pthread_cond_t)PTHREAD_COND_INITIALIZER;
        pthread_cond_wait(req->cond, rest->g_lock);
        code = _discord_request_dispatch_response(rqtor, req);
    }

    pthread_mutex_unlock(rest->g_lock);

    return code;
}
