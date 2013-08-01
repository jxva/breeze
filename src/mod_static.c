#include "http.h"
#include "common.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EXPIRE_HOURS = 87600

typedef struct _mod_static_conf {
    char root[1024];
    int  enable_list_dir;
    int  enable_etag;
    int  enable_range_req;
    
    // -1 means not set expires header; other means
    // the expiration time (in hours)
    int  expire_hours;
} mod_static_conf_t;

#define FILE_TYPE_COUNT 10

typedef struct _mime_type {
    char *content_type;
    char *exts[FILE_TYPE_COUNT];
} mime_type_t;

mime_type_t standard_types[] = {
    {"text/html",                {"html", "htm", "shtml", NULL}},
    {"text/css",                 {"css", NULL}},
    {"text/xml",                 {"xml", NULL}},
    {"text/plain",               {"txt", NULL}},
    {"image/png",                {"png", NULL}},
    {"image/gif",                {"gif", NULL}},
    {"image/tiff",               {"tif", "tiff", NULL}},
    {"image/jpeg",               {"jpg", "jpeg", NULL}},
    {"image/x-ms-bmp",           {"bmp", NULL}},
    {"image/svg+xml",            {"svg", "svgz", NULL}},
    {"application/x-javascript", {"js", NULL}}
};

static struct hsearch_data std_mime_type_hash;

static int mod_static_init();
static int static_file_write_content(request_t *req, response_t *resp, handler_ctx_t *ctx);
static int static_file_cleanup(request_t *req, response_t *resp, handler_ctx_t *ctx);
static int static_file_handler_error(response_t *resp);
static void handle_content_type(response_t *resp, const char *filepath);
static int handle_cache(request_t *req, response_t *resp,
                        const struct stat *st, const mod_static_conf_t *conf);
static char* generate_etag(const struct stat *st);


static int mod_static_init() {
    int i;
    int j;
    size_t size;
    ENTRY item, *ret;
    char **ext = NULL;

    size = sizeof(standard_types) / sizeof(standard_types[0]);

    bzero(&std_mime_type_hash, sizeof(struct hsearch_data));
    if (hcreate_r(size * 2, &std_mime_type_hash) == 0) {
        perror("Error creating standard MIME type hash");
        return -1;
    }
    for (i = 0; i < size; i++) {
        for (ext = standard_types[i].exts, j = 0;
             *ext != NULL && j < FILE_TYPE_COUNT;
             ext++, j++) {
            item.key = *ext;
            item.data = standard_types[i].content_type;
            printf("Registering standard MIME type %s:%s\n",
                   *ext, standard_types[i].content_type);
            if (hsearch_r(item, ENTER, &ret, &std_mime_type_hash) == 0) {
                fprintf(stderr, "Error entering standard MIME type\n");
            }
        }
    }
    return 0;
}

int static_file_handle(request_t *req, response_t *resp, handler_ctx_t *ctx) {
    mod_static_conf_t *conf;
    char              path[2048];
    int               fd, res;
    struct stat       st;
    size_t            len;
    ctx_state_t       val;

    conf = (mod_static_conf_t*) ctx->conf;
    len = strlen(conf->root);
    strncpy(path, conf->root, 2048);
    if (req->path[0] != '/') {
        return response_send_status(resp,
                                    STATUS_BAD_REQUEST,
                                    "Request path must starts with /");
    }
    strncat(path, req->path, 2048 - len);
    printf("Request path: %s, real file path: %s\n", req->path, path);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Resource not found");
        return static_file_handler_error(resp);
    }
    res = fstat(fd, &st);
    if (res < 0) {
        perror("Error fstat");
        return static_file_handler_error(resp);
    }
    resp->status = STATUS_OK;
    resp->content_length = st.st_size;
    handle_content_type(resp, path);
    if (handle_cache(req, resp, &st, conf)) {
        return response_send_status(resp, STATUS_NOT_MODIFIED, NULL);
    }
    
    val.as_int = fd;
    context_push(ctx, val);
    val.as_long = st.st_size;
    context_push(ctx, val);
    printf("sending headers\n");
    response_send_headers(resp, static_file_write_content);
    return HANDLER_UNFISHED;
}

static void handle_content_type(response_t *resp, const char *filepath) {
    char  *content_type = NULL, ext[20];
    int   dot_pos = -1;
    int   i;
    size_t  len = strlen(filepath);
    ENTRY   item, *ret;

    for (i = len - 1; i > 0; i--) {
        if (filepath[i] == '.') {
            dot_pos = i;
            break;
        }
    }

    if (dot_pos < 0) {
        // No '.' found in the file name (no extension part)
        return;
    }

    strncpy(ext, filepath + 1 + dot_pos, 20);
    strlowercase(ext, ext, 20);
    printf("File extension: %s\n", ext);
    
    item.key = ext;
    if (hsearch_r(item, FIND, &ret, &std_mime_type_hash) == 0) {
        return;
    }
    content_type = (char*) ret->data;
    if (content_type != NULL) {
        printf("Content type: %s\n", content_type);
        response_set_header(resp, "Content-Type", content_type); 
    }
}

static int handle_cache(request_t *req, response_t *resp,
                        const struct stat *st, const mod_static_conf_t *conf) {
    const char   *if_mod_since, *if_none_match;
    char   *etag;
    time_t  mtime, req_mtime;
    int    not_modified = 0;
    char   *buf;
    char   *cache_control;

    mtime = st->st_mtime;
    if_mod_since = request_get_header(req, "if-modified-since");
    if (if_mod_since != NULL &&
        parse_http_date(if_mod_since, &req_mtime) == 0 &&
        req_mtime == mtime) {
        printf("Resource not modified\n");
        not_modified = 1;
    }
    buf = response_alloc(resp, 32);
    format_http_date(&mtime, buf, 32);
    response_set_header(resp, "Last-Modified", buf);

    if (conf->enable_etag) {
        etag = generate_etag(st);
        if (not_modified) {
            if_none_match = request_get_header(req, "if-none-match");
            if (if_none_match == NULL ||
                strcmp(etag, if_none_match) != 0) {
                not_modified = 0;
            }
        }
        response_set_header(resp, "ETag", etag);
    }

    if (conf->expire_hours >= 0) {
        buf = response_alloc(resp, 32);
        mtime += conf->expire_hours * 3600;
        format_http_date(&mtime, buf, 32);
        response_set_header(resp, "Expires", buf);
        cache_control = response_alloc(resp, 20);
        snprintf(cache_control, 20, "max-age=%d", conf->expire_hours * 3600);
    } else {
        cache_control = "no-cache";
    }
    response_set_header(resp, "Cache-Control", cache_control);
    return not_modified;
}

static char* generate_etag(const struct stat *st) {
    char tag_buf[128];
    snprintf(tag_buf, 128, "etag-%ld-%zu", st->st_mtime, st->st_size);
    return crypt(tag_buf, "$1$breeze") + 10; // Skip the $id$salt part
}

static int static_file_write_content(request_t *req, response_t *resp, handler_ctx_t *ctx) {
    int fd;
    size_t size;

    size = context_pop(ctx)->as_long;
    fd = context_peek(ctx)->as_int;
    printf("writing file\n");
    if (response_send_file(resp, fd, 0, size, static_file_cleanup) < 0) {
        fprintf(stderr, "Error sending file\n");
        return response_send_status(resp, STATUS_NOT_FOUND,
                                    "Error sending file to client");
    }
    return HANDLER_UNFISHED;
}

static int static_file_cleanup(request_t *req, response_t *resp, handler_ctx_t *ctx) {
    int fd;

    printf("cleaning up\n");
    fd = context_pop(ctx)->as_int;
    close(fd);
    return HANDLER_DONE;
}

static int static_file_handler_error(response_t *resp) {
    switch(errno) {
    case EACCES:
    case EISDIR:
        return response_send_status(resp, STATUS_FORBIDDEN, "Access Denied");
    case ENOENT:
    default:
        return response_send_status(resp, STATUS_NOT_FOUND,
                                    "Requested resource not found");
    }
}

int main(int argc, char** args) {
    server_t *server;
    mod_static_conf_t conf;

    if (mod_static_init() < 0) {
        fprintf(stderr, "Error initializing mod_static\n");
        return -1;
    }
    
    server = server_create(8000, NULL);

    if (server == NULL) {
        fprintf(stderr, "Error creating server\n");
        return -1;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s root_dir", args[0]);
        return -2;
    }

    strncpy(conf.root, args[1], 1024);
    conf.expire_hours = 24;
    server->handler = static_file_handle;
    server->handler_conf = &conf;
    server_start(server);
    return 0;
}

