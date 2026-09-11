#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NLinkString {
  char *data;
  size_t len;
} NLinkString;

void nlink_string_free(NLinkString s);

typedef void (*NLinkProgressCb)(void *user, uint64_t remaining, uint64_t total);

int nlink_enumerate(NLinkString *out_json, NLinkString *out_err);
int nlink_open(uint8_t bus, uint8_t addr, NLinkString *out_json, NLinkString *out_err);
int nlink_close(uint8_t bus, uint8_t addr, NLinkString *out_err);
int nlink_info(uint8_t bus, uint8_t addr, NLinkString *out_json, NLinkString *out_err);
int nlink_list_dir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_json,
                   NLinkString *out_err);
int nlink_download_file(uint8_t bus, uint8_t addr, const char *remote, uint64_t size,
                        const char *dest_dir, NLinkProgressCb cb, void *user,
                        NLinkString *out_err);
int nlink_download_dir(uint8_t bus, uint8_t addr, const char *remote, const char *dest_dir,
                       NLinkProgressCb cb, void *user, NLinkString *out_err);
int nlink_upload_file(uint8_t bus, uint8_t addr, const char *dest_dir, const char *src,
                      NLinkProgressCb cb, void *user, NLinkString *out_err);
int nlink_mkdir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err);
int nlink_rm(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err);
int nlink_rmdir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err);
int nlink_move(uint8_t bus, uint8_t addr, const char *src, const char *dest,
               NLinkString *out_err);
int nlink_copy(uint8_t bus, uint8_t addr, const char *src, const char *dest,
               NLinkString *out_err);
int nlink_upload_os(uint8_t bus, uint8_t addr, const char *src, NLinkProgressCb cb, void *user,
                    NLinkString *out_err);
int nlink_backup(uint8_t bus, uint8_t addr, const char *dest, NLinkProgressCb cb, void *user,
                 NLinkString *out_err);
int nlink_restore(uint8_t bus, uint8_t addr, const char *src, NLinkProgressCb cb, void *user,
                  NLinkString *out_err);
int nlink_cli_run(void);

#ifdef __cplusplus
}
#endif
