//
// Created by Giuseppe Coviello on 11/20/26.
//

#include "prg32_fs.h"

#include "prg32.h"

#include "prg32_http_internal.h"

#include <dirent.h>
#include <esp_log.h>
#include <esp_littlefs.h>
#include <string.h>
#include <sys/unistd.h>

static const char *TAG = "prg32_fs";

#define OPEN_FILE_LIMIT 4
#define NAME_LENGTH_LIMIT 12

#define STORAGE_DIR "/storage"
#define FULL_PATH_LENGTH (12 + NAME_LENGTH_LIMIT)

static FILE *open_files[OPEN_FILE_LIMIT];

int8_t prg32_fs_open(const char *filename, const char *mode) {
    if (strlen(filename) > NAME_LENGTH_LIMIT) {
        ESP_LOGE(TAG, "File name '%s' too long", filename);
        return PRG32_FS_ERR_NAME_LENGTH;
    }

    int8_t fd;
    for (fd = 0; fd < OPEN_FILE_LIMIT; fd++) {
        if (open_files[fd] == NULL) {
            break;
        }
    }
    if (fd == OPEN_FILE_LIMIT) {
        ESP_LOGE(TAG, "Too many files open");
        return PRG32_FS_ERR_TOO_MANY_OPENFILES;
    }

    static char full_filename[FULL_PATH_LENGTH];
    snprintf(full_filename, sizeof(full_filename), "%s/%s", STORAGE_DIR, filename);
    ESP_LOGI(TAG, "Opening file '%s'", full_filename);

    open_files[fd] = fopen(full_filename, mode);
    if (open_files[fd] == NULL) {
        ESP_LOGE(TAG, "Failed to open file '%s'", filename);
        return PRG32_FS_ERR_FILE_NOT_FOUND;
    }

    return fd;
}

int32_t prg32_fs_read(const int8_t fd, void *buf, const uint16_t count) {
    if (fd < 0 || fd >= OPEN_FILE_LIMIT || open_files[fd] == NULL) {
        ESP_LOGE(TAG, "Invalid file descriptor: %d", fd);
        return PRG32_FS_ERR_INVALID_FILE;
    }
    ESP_LOGI(TAG, "Reading %u bytes from file \"%d\"", count, fd);
    int32_t r = (int32_t) fread(buf, 1, count, open_files[fd]);
    ESP_LOGI(TAG, "Reading result: %ld", r);
    return r;
}

int32_t prg32_fs_write(const int8_t fd, const void *buf, const uint16_t count) {
    if (fd < 0 || fd >= OPEN_FILE_LIMIT || open_files[fd] == NULL) {
        ESP_LOGE(TAG, "Invalid file descriptor: %d", fd);
        return PRG32_FS_ERR_INVALID_FILE;
    }
    return (int32_t) fwrite(buf, 1, count, open_files[fd]);
}

prg32_fs_error_t prg32_fs_close(const int8_t fd) {
    if (fd < 0 || fd >= OPEN_FILE_LIMIT || open_files[fd] == NULL) {
        ESP_LOGE(TAG, "Invalid file descriptor: %d", fd);
        return PRG32_FS_ERR_INVALID_FILE;
    }
    fclose(open_files[fd]);
    open_files[fd] = NULL;
    return PRG32_FS_OK;
}

void prg32_fs_init() {
    ESP_LOGI(TAG, "Initializing filesystem");

    const esp_vfs_littlefs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    // Use settings defined above to initialize and mount LittleFS filesystem.
    // Note: esp_vfs_littlefs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    FILE *f = fopen("/storage/test.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
    } else {
        fprintf(f, "HELLO!\n");
        fclose(f);
    }

    DIR *d = opendir("/storage");
    if (d == NULL) {
        ESP_LOGE(TAG, "Failed to open directory");
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "FILE %s", e->d_name);
        }
        closedir(d);
    }
}

//
// This will handle a file to upload to persistent filesystem
// Filename is in the query filename=
// There is an optional overwrite= (true or false), to indicate if to overwrite a file or not, default is false
//
esp_err_t fs_upload_file_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /api/fs content_len=%u", (unsigned)req->content_len);
    if (req->content_len == 0) {
        ESP_LOGW(TAG, "Body not provided for POST /api/fs");
        httpd_resp_send_err(req, 400, "body not provided");
        return ESP_FAIL;
    }

    uint16_t query_len = (uint16_t) httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        // if there is no query, no filename was provided hence we return an error right here
        ESP_LOGW(TAG, "Query not provided for POST /api/fs");
        httpd_resp_send_err(req, 400, "filename not provided");
        return ESP_FAIL;
    }
    query_len += 1;

#define FS_UPLOAD_MAX_QUERY_LEN 256
    if (query_len >= FS_UPLOAD_MAX_QUERY_LEN) {
        ESP_LOGW(TAG, "Query for POST /api/fs too long: %u (maximum: %u)", query_len, FS_UPLOAD_MAX_QUERY_LEN);
        httpd_resp_send_err(req, 400, "query too long");
        return ESP_FAIL;
    }

    char *query = heap_caps_malloc(query_len, MALLOC_CAP_8BIT);
    esp_err_t err = httpd_req_get_url_query_str(req, query, query_len);
    if (err != ESP_OK) {
        heap_caps_free(query);
        ESP_LOGE(TAG, "Cannot obtain query string: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // the +2 is to check if the filename is too long
    char filename[NAME_LENGTH_LIMIT + 2];
    err = httpd_query_key_value(query, "filename", filename, sizeof(filename));
    if (err != ESP_OK) {
        heap_caps_free(query);
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Filename not provided for POST /api/fs");
            httpd_resp_send_err(req, 400, "filename not provided");
            return ESP_FAIL;
        }
        ESP_LOGE(TAG, "Failed to obtain query string: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (strlen(filename) > NAME_LENGTH_LIMIT) {
        heap_caps_free(query);
        ESP_LOGW(TAG, "Filename too long '%s'", filename);
        httpd_resp_send_err(req, 400, "filename too long");
        return ESP_FAIL;
    }

    bool overwrite = false;
    char overwrite_value[6];
    err = httpd_query_key_value(query, "overwrite", overwrite_value, sizeof(overwrite_value));
    // we can free query now since no other parameters is needed
    heap_caps_free(query);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to overwrite query string: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        // not found, we keep default
    } else {
        if (strncasecmp(overwrite_value, "true", 4) != 0 && strncasecmp(overwrite_value, "false", 5) != 0) {
            ESP_LOGW(TAG, "Invalid overwrite value '%s'", overwrite_value);
            httpd_resp_send_err(req, 400, "invalid overwrite value");
            return ESP_FAIL;
        }
        overwrite = strncmp(overwrite_value, "true", 4) == 0;
    }

    // TODO: we can avoid this double allocation for the filename
    char full_filename[FULL_PATH_LENGTH];
    snprintf(full_filename, sizeof(full_filename), STORAGE_DIR "/%s", filename);

    // we are not allowed to overwrite, let's check if file exists
    if (!overwrite) {
        FILE *file = fopen(full_filename, "r");
        if (file != NULL) {
            ESP_LOGW(TAG, "File '%s' already exists", full_filename);
            httpd_resp_send_err(req, 400, "file already exists");
            return ESP_FAIL;
        }
        fclose(file);
    }

    FILE *f = fopen(full_filename, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file '%s'", full_filename);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t *chunk = heap_caps_malloc(PRG32_HTTP_UPLOAD_CHUNK_BYTES,
                                      MALLOC_CAP_8BIT);
    if (!chunk) {
        fclose(f);
        unlink(full_filename);
        ESP_LOGE(TAG, "Cannot allocate chunk buffer");
        httpd_resp_send_err(req, 500, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    while (written < req->content_len) {
        int received = httpd_req_recv(req, (char *) chunk, PRG32_HTTP_UPLOAD_CHUNK_BYTES);
        if (received < 0) {
            fclose(f);
            unlink(full_filename);
            ESP_LOGE(TAG, "httpd_req_recv failed");
            httpd_resp_send_500(req);
            heap_caps_free(chunk);
            return ESP_FAIL;
        }
        if (received == 0) {
            break;
        }
        fwrite(chunk, 1, received, f);
        written += received;
    }

    heap_caps_free(chunk);
    fclose(f);

    if (written < req->content_len) {
        unlink(full_filename);
        ESP_LOGW(TAG, "Not whole file received: %u instead of %u", written, req->content_len);    \
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_send(req, "OK", 3);

    return ESP_OK;
}