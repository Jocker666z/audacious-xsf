#include <glib.h>
#include <cstdlib>

#define WANT_VFS_STDIO_COMPAT

#include <libaudcore/plugin.h>

#include "plugin.h"
#include "vfs.h"

static void * psf_file_fopen( void * context, const char * uri )
{
    VFSFile *vfsFile = new VFSFile(uri, "rb");
    if (!vfsFile || !*vfsFile) {
        delete vfsFile;
        return NULL;
    }

    return vfsFile;
}

static size_t psf_file_fread( void * buffer, size_t size, size_t count, void * handle )
{
    VFSFile *vfsFile = (VFSFile *)handle;
    if (vfsFile) return vfsFile->fread( buffer, size, count );
    else return 0;
}

static int psf_file_fseek( void * handle, int64_t offset, int whence )
{
    VFSFile *vfsFile = (VFSFile *)handle;
    if (vfsFile) return vfsFile->fseek( offset, to_vfs_seek_type(whence) );
    else return -1;
}

static int psf_file_fclose( void * handle )
{
    VFSFile *vfsFile = (VFSFile *)handle;
    delete vfsFile;
    return 0;
}

static long psf_file_ftell( void * handle )
{
    VFSFile *vfsFile = (VFSFile *)handle;
    if (vfsFile) return vfsFile->ftell();
    else return 0;
}

static const psf_file_callbacks psf_file_system =
{
    "\\/|:",
    NULL,
    psf_file_fopen,
    psf_file_fread,
    psf_file_fseek,
    psf_file_fclose,
    psf_file_ftell
};

int psf_load_vfs( const char * uri, uint8_t allowed_version,
                  psf_load_callback load_target, void * load_context, psf_info_callback info_target,
                  void * info_context, int info_want_nested_tags, psf_status_callback status_target,
                  void * status_context) {
    return psf_load( uri, &psf_file_system, allowed_version, load_target, load_context, info_target, info_context, info_want_nested_tags, status_target, status_context);
}

