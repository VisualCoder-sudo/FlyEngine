// flyvfs.h -- hooks that let the engine serve files from a virtual file system (the archive appended to an
// exported game's executable) through raylib's file functions: FileExists, DirectoryExists, LoadFileData
// and LoadFileText, which every image/model/font loader goes through.
#ifndef FLYVFS_H
#define FLYVFS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlyVfsHooks {
    int (*handles)(const char* path);                   // does the VFS own this path?
    int (*fileExists)(const char* path);                // regular file inside the VFS
    int (*dirExists)(const char* path);                 // directory inside the VFS
    unsigned char* (*readFile)(const char* path, int* size);   // malloc()ed bytes (freed by UnloadFileData), or NULL
} FlyVfsHooks;

// Installs (or, with NULL, removes) the hooks. The pointer must stay valid.
void FlySetVfsHooks(const FlyVfsHooks* hooks);

#ifdef __cplusplus
}
#endif
#endif
