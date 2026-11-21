//
// Created by Giuseppe Coviello on 11/20/26.
//

#ifndef PRG32_PRG32_FS_H
#define PRG32_PRG32_FS_H

#include <esp_http_server.h>

void prg32_fs_init();

// HTTP handler for uploading a file
esp_err_t fs_upload_file_handler(httpd_req_t *req);

#endif //PRG32_PRG32_FS_H
