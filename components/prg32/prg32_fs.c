//
// Created by Giuseppe Coviello on 11/20/26.
//

#include "prg32_fs.h"

#include "prg32.h"

#include <dirent.h>
#include <esp_log.h>
#include <esp_littlefs.h>
#include <string.h>

static const char *TAG = "prg32_fs";

#define OPEN_FILE_LIMIT 4
#define NAME_LENGTH_LIMIT 12

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

    static char full_filename[8 + NAME_LENGTH_LIMIT + 1];
    snprintf(full_filename, sizeof(full_filename), "/storage/%s", filename);
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
