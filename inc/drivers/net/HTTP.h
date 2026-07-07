#pragma once
#include <stdint.h>
uint32_t http_get_buffer(const char* hostname, const char* path, char* out_buf, uint32_t max_len);
uint32_t http_post_buffer(const char* hostname, const char* path, const char* post_data, char* out_buf, uint32_t max_len);