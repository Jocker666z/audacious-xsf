#ifndef __VFS__
#define __VFS__

#include "psflib/psflib.h"

int psf_load_vfs( const char * uri, uint8_t allowed_version,
              psf_load_callback load_target, void * load_context, psf_info_callback info_target,
              void * info_context, int info_want_nested_tags, psf_status_callback status_target,
              void * status_context);

#endif
