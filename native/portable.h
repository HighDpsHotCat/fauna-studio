#ifndef AA_PORTABLE_H
#define AA_PORTABLE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <stdatomic.h>
#include <limits.h>
#include <errno.h>
typedef struct {atomic_int signaled;} FsCancel;
typedef struct {unsigned long value;} FsId;
typedef struct {unsigned wYear,wMonth,wDay,wHour,wMinute,wSecond;} FsTime;
#define FS_FAILED(x) ((x)<0)
#define FS_SUCCEEDED(x) ((x)>=0)
#define CP_UTF8 65001
#define WAIT_OBJECT_0 0
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define ERROR_SUCCESS 0
#define ERROR_ALREADY_EXISTS EEXIST
#define ERROR_FILE_EXISTS EEXIST
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 0
#define _TRUNCATE SIZE_MAX
void fs_init(void);
int fs_wformat(wchar_t*,size_t,const wchar_t*,...);
int fs_wcopy(wchar_t*,size_t,const wchar_t*);
int fs_wncopy(wchar_t*,size_t,const wchar_t*,size_t);
int fs_wcat(wchar_t*,size_t,const wchar_t*);
int fs_decode_utf8(unsigned,unsigned,const char*,int,wchar_t*,int);
int fs_encode_utf8(unsigned,unsigned,const wchar_t*,int,char*,int,void*,void*);
FILE *fs_wfopen(const wchar_t*,const wchar_t*);
int fs_exists(const wchar_t*);
uint32_t fs_attributes(const wchar_t*);
int fs_parent(wchar_t*);
int fs_mkdir(const wchar_t*,void*);
int fs_mkdirs(void*,const wchar_t*,void*);
int fs_rmdir(const wchar_t*);
int fs_unlink(const wchar_t*);
int fs_copyfile(const wchar_t*,const wchar_t*,int);
int fs_move(const wchar_t*,const wchar_t*,unsigned);
uint32_t fs_absolute(const wchar_t*,uint32_t,wchar_t*,void*);
int fs_create_id(FsId*);
void fs_utc_time(FsTime*);
FsCancel *fs_cancel_create(void*,int,int,void*);
void fs_cancel_set(FsCancel*);
void fs_cancel_free(FsCancel*);
int fs_cancel_wait(FsCancel*,unsigned);
uint64_t fs_milliseconds(void);
int fs_hash_hex(const void*,size_t,int,char*);
int fs_sha256_file(const wchar_t*,char[65]);
static inline int fs_wtoi(const wchar_t *s){return (int)wcstol(s,NULL,10);}
static inline int tmpfile_s(FILE **f){*f=tmpfile();return *f?0:errno;}
#endif
