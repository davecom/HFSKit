// hfswrapper.c
// C implementation of a small libhfs wrapper for use from Swift.

#include "hfswrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* libhfs header from hfsutils (must be in your include path). */
#include "libhfs.h"
#include "low.h"
#include "volume.h"
#include "copyin.h"
#include "hcopy.h"

/* ---- Internal helpers -------------------------------------------------- */

int hfs_debug_logging_enabled = 1;

static const char *
normalize_error_detail(const char *detail)
{
    if (!detail || detail[0] == '\0') {
        return NULL;
    }
    if (strcmp(detail, "no error") == 0) {
        return NULL;
    }
    return detail;
}

static HFSWError
hfsw_ok(void)
{
    HFSWError err;
    err.code = 0;
    err.detail = NULL;
    return err;
}

static HFSWError
hfsw_err(const char *detail)
{
    HFSWError err;
    err.code = errno ? errno : EIO;
    err.detail = normalize_error_detail(detail);
    return err;
}

static HFSWError
resolve_hfs_partno_from_map_index(const char *path,
                                  int requestedPartno,
                                  int *outResolvedPartno)
{
    if (!path || !outResolvedPartno) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    if (requestedPartno <= 0) {
        *outResolvedPartno = requestedPartno;
        return hfsw_ok();
    }

    hfsvol vol;
    v_init(&vol, HFS_OPT_NOCACHE);
    if (v_open(&vol, path, HFS_MODE_RDONLY) == -1) {
        return hfsw_err(hfs_error);
    }

    Partition map;
    if (l_getpmentry(&vol, &map, 1) == -1) {
        v_close(&vol);
        return hfsw_err(hfs_error);
    }

    /* No valid partition map: let libhfs handle direct partition semantics. */
    if (map.pmSig != HFS_PM_SIGWORD) {
        v_close(&vol);
        *outResolvedPartno = requestedPartno;
        return hfsw_ok();
    }

    unsigned long total = (unsigned long)map.pmMapBlkCnt;
    int hfsOrdinal = 0;
    int found = 0;

    for (unsigned long bnum = 1; bnum <= total; ++bnum) {
        if (l_getpmentry(&vol, &map, bnum) == -1) {
            v_close(&vol);
            return hfsw_err(hfs_error);
        }

        if (strcmp((const char *)map.pmParType, "Apple_HFS") == 0) {
            ++hfsOrdinal;
        }

        if ((int)bnum == requestedPartno) {
            found = 1;
            break;
        }
    }

    if (v_close(&vol) == -1) {
        return hfsw_err(hfs_error);
    }

    if (!found || hfsOrdinal == 0) {
        errno = EINVAL;
        return hfsw_err("selected partition is not an HFS partition");
    }

    *outResolvedPartno = hfsOrdinal;
    return hfsw_ok();
}

static void
fill_file_info(const hfsdirent *e, HFSWFileInfo *out)
{
    memset(out, 0, sizeof(*out));

    /* Name (HFS_MAX_FLEN is 31; we have 255 bytes) */
    strncpy(out->name, e->name, sizeof(out->name) - 1);

    out->isDirectory  = (e->flags & HFS_ISDIR) ? 1 : 0;
    out->dataForkSize = (uint32_t)e->u.file.dsize;
    out->rsrcForkSize = (uint32_t)e->u.file.rsize;

    /* Type/creator are 4 chars + NUL in hfsdirent */
    strncpy(out->fileType,   e->u.file.type,    4);
    out->fileType[4] = '\0';
    strncpy(out->fileCreator, e->u.file.creator, 4);
    out->fileCreator[4] = '\0';

    out->flags    = (uint16_t)e->fdflags;
    out->created  = e->crdate;
    out->modified = e->mddate;
}

/* ---- Public API -------------------------------------------------------- */

HFSImage *
hfsw_open_image(const char *path, int readWrite)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    int mode = readWrite ? 1 : 0; /* HFS_MODE_RDWR / HFS_MODE_RDONLY; libhfs uses 0/1 */
    hfsvol *vol = hfs_mount((char *)path, 0, mode);
    if (!vol) {
        /* libhfs sets errno / hfs_error */
        return NULL;
    }

    HFSImage *img = (HFSImage *)malloc(sizeof(HFSImage));
    if (!img) {
        hfs_umount(vol);
        errno = ENOMEM;
        return NULL;
    }

    img->vol = vol;
    return img;
}

HFSWOpenResult
hfsw_open_image_ex(const char *path, int readWrite, int partno)
{
    HFSWOpenResult result;
    result.image = NULL;
    result.error = hfsw_ok();

    if (!path) {
        errno = EINVAL;
        result.error = hfsw_err(NULL);
        return result;
    }

    if (partno < 0) {
        errno = EINVAL;
        result.error = hfsw_err(NULL);
        return result;
    }

    int resolvedPartno = 0;
    result.error = resolve_hfs_partno_from_map_index(path, partno, &resolvedPartno);
    if (result.error.code != 0) {
        return result;
    }

    int mode = readWrite ? 1 : 0; /* HFS_MODE_RDWR / HFS_MODE_RDONLY; libhfs uses 0/1 */
    hfsvol *vol = hfs_mount((char *)path, resolvedPartno, mode);
    if (!vol) {
        result.error = hfsw_err(hfs_error);
        return result;
    }

    HFSImage *img = (HFSImage *)malloc(sizeof(HFSImage));
    if (!img) {
        hfs_umount(vol);
        errno = ENOMEM;
        result.error = hfsw_err(NULL);
        return result;
    }

    img->vol = vol;
    result.image = img;
    return result;
}

void
hfsw_close_image(HFSImage *image)
{
    if (!image) return;

    if (image->vol) {
        hfs_umount(image->vol);
        image->vol = NULL;
    }
    free(image);
}

HFSWError
hfsw_create_blank_image(const char *path,
                        uint64_t sizeBytes,
                        const char *volumeName)
{
    if (!path || !volumeName) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    if (sizeBytes == 0) {
        errno = EINVAL;
        return hfsw_err("image size must be greater than zero");
    }

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
        return hfsw_err("error creating image file");
    }

    if (ftruncate(fd, (off_t)sizeBytes) == -1) {
        int saved = errno;
        close(fd);
        errno = saved;
        return hfsw_err("error sizing image file");
    }

    if (close(fd) == -1) {
        return hfsw_err("error closing image file");
    }

    if (hfs_format(path, 0, HFS_OPT_NOCACHE, volumeName, 0, NULL) == -1) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

void
hfsw_set_debug_logging(int enabled)
{
    hfs_debug_logging_enabled = enabled ? 1 : 0;
}

int
hfsw_get_debug_logging(void)
{
    return hfs_debug_logging_enabled;
}

HFSWError
hfsw_list_partitions(const char *path,
                     hfsw_partition_callback callback,
                     void *context,
                     int *outHasPartitionMap)
{
    if (!path || !callback) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    if (outHasPartitionMap) {
        *outHasPartitionMap = 0;
    }

    hfsvol vol;
    v_init(&vol, HFS_OPT_NOCACHE);

    if (v_open(&vol, path, HFS_MODE_RDONLY) == -1) {
        return hfsw_err(hfs_error);
    }

    Partition map;
    if (l_getpmentry(&vol, &map, 1) == -1 ||
        map.pmSig != HFS_PM_SIGWORD) {
        v_close(&vol);
        return hfsw_ok();
    }

    if (outHasPartitionMap) {
        *outHasPartitionMap = 1;
    }

    unsigned long total = (unsigned long)map.pmMapBlkCnt;
    for (unsigned long bnum = 1; bnum <= total; ++bnum) {
        if (l_getpmentry(&vol, &map, bnum) == -1) {
            v_close(&vol);
            return hfsw_err(hfs_error);
        }

        HFSWPartitionInfo info;
        memset(&info, 0, sizeof(info));
        info.index = (int)bnum;
        strncpy(info.name, (const char *)map.pmPartName, sizeof(info.name) - 1);
        strncpy(info.type, (const char *)map.pmParType, sizeof(info.type) - 1);
        info.startBlock = (uint32_t)map.pmPyPartStart;
        info.blockCount = (uint32_t)map.pmPartBlkCnt;
        info.dataStart = (uint32_t)map.pmLgDataStart;
        info.dataCount = (uint32_t)map.pmDataCnt;
        info.isHFS = (strcmp(info.type, "Apple_HFS") == 0) ? 1 : 0;
        callback(&info, context);
    }

    if (v_close(&vol) == -1) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_stat(HFSImage *image,
          const char *hfsPath,
          HFSWFileInfo *outInfo)
{
    if (!image || !image->vol || !hfsPath || !outInfo) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    hfsdirent ent;
    if (hfs_stat(image->vol, (char *)hfsPath, &ent) != 0) {
        /* errno set by libhfs */
        return hfsw_err(hfs_error);
    }

    fill_file_info(&ent, outInfo);
    return hfsw_ok();
}

HFSWError
hfsw_list_dir(HFSImage *image,
              const char *hfsDirPath,
              hfsw_list_callback callback,
              void *context)
{
    if (!image || !image->vol || !callback) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    const char *path = (hfsDirPath && hfsDirPath[0]) ? hfsDirPath : ":";

    hfsdir *dir = hfs_opendir(image->vol, (char *)path);
    if (!dir) {
        /* errno set by libhfs */
        return hfsw_err(hfs_error);
    }

    hfsdirent ent;
    while (hfs_readdir(dir, &ent) == 0) {
        HFSWFileInfo info;
        fill_file_info(&ent, &info);
        callback(&info, context);
    }

    hfs_closedir(dir);
    return hfsw_ok();
}

HFSWError
hfsw_volume_info(HFSImage *image,
                 HFSWVolumeInfo *outInfo)
{
    if (!image || !image->vol || !outInfo) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    hfsvolent ent;
    if (hfs_vstat(image->vol, &ent) != 0) {
        return hfsw_err(hfs_error);
    }

    memset(outInfo, 0, sizeof(*outInfo));
    strncpy(outInfo->name, ent.name, sizeof(outInfo->name) - 1);
    outInfo->flags = (uint32_t)ent.flags;
    outInfo->totalBytes = (uint64_t)ent.totbytes;
    outInfo->freeBytes = (uint64_t)ent.freebytes;
    outInfo->allocationBlockSize = (uint32_t)ent.alblocksz;
    outInfo->clumpSize = (uint32_t)ent.clumpsz;
    outInfo->numberOfFiles = (uint32_t)ent.numfiles;
    outInfo->numberOfDirectories = (uint32_t)ent.numdirs;
    outInfo->created = ent.crdate;
    outInfo->modified = ent.mddate;
    outInfo->backup = ent.bkdate;
    outInfo->blessedFolderId = (uint32_t)ent.blessed;

    return hfsw_ok();
}

HFSWError
hfsw_delete(HFSImage *image,
            const char *hfsPath)
{
    if (!image || !image->vol || !hfsPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    hfsdirent ent;
    if (hfs_stat(image->vol, (char *)hfsPath, &ent) != 0) {
        return hfsw_err(hfs_error);
    }

    if (ent.flags & HFS_ISDIR) {
        if (hfs_rmdir(image->vol, (char *)hfsPath) != 0) {
            return hfsw_err(hfs_error);
        }
    } else {
        if (hfs_delete(image->vol, (char *)hfsPath) != 0) {
            return hfsw_err(hfs_error);
        }
    }

    return hfsw_ok();
}

HFSWError
hfsw_rename(HFSImage *image,
            const char *hfsOldPath,
            const char *newName)
{
    if (!image || !image->vol || !hfsOldPath || !newName) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    /* hfs_rename takes (vol, oldPath, newName) where newName is a bare name. */
    if (hfs_rename(image->vol, (char *)hfsOldPath, (char *)newName) != 0) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_move(HFSImage *image,
          const char *hfsOldPath,
          const char *newParentDirectory)
{
    if (!image || !image->vol || !hfsOldPath || !newParentDirectory) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    if (newParentDirectory[0] == '\0') {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    const char *baseName = strrchr(hfsOldPath, ':');
    baseName = baseName ? baseName + 1 : hfsOldPath;
    if (baseName[0] == '\0') {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    size_t parentLen = strlen(newParentDirectory);
    int needsSep = (parentLen > 0 && newParentDirectory[parentLen - 1] != ':');
    size_t destLen = parentLen + (needsSep ? 1 : 0) + strlen(baseName) + 1;

    char *destPath = (char *)malloc(destLen);
    if (!destPath) {
        errno = ENOMEM;
        return hfsw_err(NULL);
    }

    if (needsSep) {
        snprintf(destPath, destLen, "%s:%s", newParentDirectory, baseName);
    } else {
        snprintf(destPath, destLen, "%s%s", newParentDirectory, baseName);
    }

    int result = hfs_rename(image->vol, (char *)hfsOldPath, destPath);
    free(destPath);
    if (result != 0) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_mkdir(HFSImage *image,
           const char *hfsDirPath)
{
    if (!image || !image->vol || !hfsDirPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    if (hfs_mkdir(image->vol, (char *)hfsDirPath) != 0) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

/* Helper: tidy 4-char Mac type/creator into char[5]. */
static void
normalize_fourcc(const char *in, char out[5])
{
    size_t len = in ? strlen(in) : 0;
    size_t i;

    for (i = 0; i < 4; ++i) {
        if (i < len) {
            out[i] = in[i];
        } else {
            out[i] = ' ';
        }
    }
    out[4] = '\0';
}

static int
hfsw_mode_to_hcopy_mode(int mode)
{
    switch (mode) {
        case HFSW_COPY_MODE_AUTO:
            return 'a';
        case HFSW_COPY_MODE_RAW:
            return 'r';
        case HFSW_COPY_MODE_MACB:
            return 'm';
        case HFSW_COPY_MODE_BINH:
            return 'b';
        case HFSW_COPY_MODE_TEXT:
            return 't';
        default:
            return 0;
    }
}

HFSWError
hfsw_copy_in(HFSImage *image,
             const char *hostPath,
             const char *hfsDestPath,
             int mode)
{
    if (!image || !image->vol || !hostPath || !hfsDestPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    int hcopyMode = hfsw_mode_to_hcopy_mode(mode);
    if (hcopyMode == 0) {
        errno = EINVAL;
        return hfsw_err("unsupported copy mode");
    }

    char *sources[1];
    sources[0] = (char *)hostPath;

    if (do_copyin(image->vol, 1, sources, hfsDestPath, hcopyMode) != 0) {
        return hfsw_err(cpi_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_copy_out(HFSImage *image,
              const char *hfsPath,
              const char *hostDestPath,
              int mode)
{
    if (!image || !image->vol || !hfsPath || !hostDestPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    int hcopyMode = hfsw_mode_to_hcopy_mode(mode);
    if (hcopyMode == 0) {
        errno = EINVAL;
        return hfsw_err("unsupported copy mode");
    }

    char *sources[1];
    sources[0] = (char *)hfsPath;

    if (do_copyout(image->vol, 1, sources, hostDestPath, hcopyMode) != 0) {
        return hfsw_err(cpo_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_set_type_creator(HFSImage *image,
                      const char *hfsPath,
                      const char *fileType,
                      const char *fileCreator)
{
    if (!image || !image->vol || !hfsPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    hfsdirent ent;
    if (hfs_stat(image->vol, (char *)hfsPath, &ent) != 0) {
        return hfsw_err(hfs_error);
    }

    normalize_fourcc(fileType,   ent.u.file.type);
    normalize_fourcc(fileCreator, ent.u.file.creator);

    if (hfs_setattr(image->vol, (char *)hfsPath, &ent) != 0) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}

HFSWError
hfsw_set_blessed(HFSImage *image,
                 const char *hfsPath)
{
    if (!image || !image->vol || !hfsPath) {
        errno = EINVAL;
        return hfsw_err(NULL);
    }

    hfsdirent dirEnt;
    if (hfs_stat(image->vol, (char *)hfsPath, &dirEnt) != 0) {
        return hfsw_err(hfs_error);
    }

    if (!(dirEnt.flags & HFS_ISDIR)) {
        errno = ENOTDIR;
        return hfsw_err("blessed path must be a directory");
    }

    hfsvolent volEnt;
    if (hfs_vstat(image->vol, &volEnt) != 0) {
        return hfsw_err(hfs_error);
    }

    volEnt.blessed = dirEnt.cnid;
    if (hfs_vsetattr(image->vol, &volEnt) != 0) {
        return hfsw_err(hfs_error);
    }

    return hfsw_ok();
}
