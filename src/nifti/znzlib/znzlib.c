/** \file znzlib.c
    \brief Low level i/o interface to compressed and noncompressed files.
        Written by Mark Jenkinson, FMRIB

This library provides an interface to both compressed (gzip/zlib) and
uncompressed (normal) file IO.  The functions are written to have the
same interface as the standard file IO functions.

To use this library instead of normal file IO, the following changes
are required:
 - replace all instances of FILE* with znzFile
 - change the name of all function calls, replacing the initial character
   f with the znz  (e.g. fseek becomes znzseek)
   one exception is rewind() -> znzrewind()
 - add a third parameter to all calls to znzopen (previously fopen)
   that specifies whether to use compression (1) or not (0)
 - use znz_isnull rather than any (pointer == NULL) comparisons in the code
   for znzfile types (normally done after a return from znzopen)

NB: seeks for writable files with compression are quite restricted

 */

#include "znzlib.h"
#include "znzlib_version.h"

#include <errno.h>
#include <sys/wait.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

static int znz_copy_stream(FILE *src, FILE *dst)
{
  char buf[1<<15];
  size_t nread;

  if( src == NULL || dst == NULL ) return -1;

  while( (nread = fread(buf, 1, sizeof(buf), src)) > 0 ) {
    if( fwrite(buf, 1, nread, dst) != nread ) return -1;
  }

  if( ferror(src) ) return -1;
  return 0;
}

static int znz_make_tmpfile(char *tname, size_t tsize, FILE **fp)
{
  int tfd;

  if( tname == NULL || tsize < 16 || fp == NULL ) return -1;

  snprintf(tname, tsize, "/tmp/znzlibXXXXXX");
  tfd = mkstemp(tname);
  if( tfd < 0 ) return -1;

  *fp = fdopen(tfd, "wb+");
  if( *fp == NULL ) {
    close(tfd);
    unlink(tname);
    return -1;
  }

  return 0;
}

static char *znz_shell_quote_single(const char *s)
{
  size_t i, len, out_len;
  char *q, *p;

  if( s == NULL ) return NULL;
  len = strlen(s);
  out_len = 3; /* opening/closing quote + NUL */
  for( i = 0 ; i < len ; ++i ) out_len += (s[i] == '\'') ? 4 : 1;

  q = (char *)malloc(out_len);
  if( q == NULL ) return NULL;

  p = q;
  *p++ = '\'';
  for( i = 0 ; i < len ; ++i ) {
    if( s[i] == '\'' ) {
      *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
    } else {
      *p++ = s[i];
    }
  }
  *p++ = '\'';
  *p = '\0';

  return q;
}

static int znz_force_zstd_decode_all_cmd(znzFile file)
{
  char *qpath = NULL;
  char *cmd = NULL;
  FILE *pipe = NULL;
  int status;

  if( file == NULL || file->zpath == NULL || file->nzfptr == NULL || file->ztmpname == NULL ) return -1;

  if( getenv("ZNZ_DEBUG_ZSTD") )
    fprintf(stderr,"++ ZNZDBG: cmd-decode begin for %s\n", file->zpath);

  qpath = znz_shell_quote_single(file->zpath);
  if( qpath == NULL ) return -1;

  cmd = (char *)malloc(strlen(qpath) + 64);
  if( cmd == NULL ) {
    free(qpath);
    return -1;
  }
  sprintf(cmd, "zstd -dc -- %s", qpath);
  free(qpath);

  file->nzfptr = freopen(file->ztmpname, "wb+", file->nzfptr);
  if( file->nzfptr == NULL ) {
    free(cmd);
    return -1;
  }

  pipe = popen(cmd, "r");
  free(cmd);
  if( pipe == NULL ) return -1;

  if( znz_copy_stream(pipe, file->nzfptr) ) {
    pclose(pipe);
    return -1;
  }

  status = pclose(pipe);
  if( status != 0 ) {
    if( getenv("ZNZ_DEBUG_ZSTD") )
      fprintf(stderr,"++ ZNZDBG: cmd-decode pclose status=%d\n", status);
    return -1;
  }

  fflush(file->nzfptr);
  if( fseek(file->nzfptr, 0, SEEK_END) == 0 ) {
    file->zfilled = (znz_off_t)ftell(file->nzfptr);
    file->zusize = file->zfilled;
    file->zsize_known = 1;
  }
  file->zeof = 1;
  file->zstream = 0;
  if( file->zsrcfptr != NULL ) {
    fseek(file->zsrcfptr, 0, SEEK_END);
  }
  file->zstin_size = 0;
  file->zstin_pos = 0;

  if( getenv("ZNZ_DEBUG_ZSTD") )
    fprintf(stderr,"++ ZNZDBG: cmd-decode done, zfilled=%lld\n", (long long)file->zfilled);

  return 0;
}

static int znz_stage_zstd_write(const char *path, FILE *src)
{
  int status = -1;

  if( path == NULL || src == NULL ) return -1;

#ifdef HAVE_ZSTD
  ZSTD_CStream *zs = NULL;
  FILE *dst = NULL;
  char *zthr_env = NULL;
  znz_off_t src_size = -1;
  long ncpu = 1;
  long zstd_threads = 1;
  size_t in_cap, out_cap;
  unsigned char *in_buf = NULL, *out_buf = NULL;

  dst = fopen(path, "wb");
  if( dst == NULL ) goto ZW_cleanup;

  zs = ZSTD_createCStream();
  if( zs == NULL ) goto ZW_cleanup;

  if( fseek(src, 0, SEEK_END) == 0 ) {
    src_size = (znz_off_t)ftell(src);
  }

  /* default to all online cores for zstd writes; allow override */
  ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if( ncpu < 1 ) ncpu = 1;
  zstd_threads = ncpu;
  zthr_env = getenv("AFNI_ZSTD_THREADS");
  if( zthr_env != NULL && *zthr_env != '\0' ) {
    long ztmp = strtol(zthr_env, NULL, 10);
    if( ztmp >= 0 ) zstd_threads = ztmp;
  }

  if( ZSTD_isError(ZSTD_initCStream(zs, 1)) ) goto ZW_cleanup;
#if ZSTD_VERSION_NUMBER >= 10304
  if( zstd_threads > 0 ) {
    if( ZSTD_isError(ZSTD_CCtx_setParameter((ZSTD_CCtx *)zs, ZSTD_c_nbWorkers, (int)zstd_threads)) ) goto ZW_cleanup;
  }
#endif
  if( src_size >= 0 ) {
    if( ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize((ZSTD_CCtx *)zs, (unsigned long long)src_size)) ) goto ZW_cleanup;
  }

  in_cap = ZSTD_CStreamInSize();
  out_cap = ZSTD_CStreamOutSize();
  in_buf = (unsigned char *)malloc(in_cap);
  out_buf = (unsigned char *)malloc(out_cap);
  if( in_buf == NULL || out_buf == NULL ) goto ZW_cleanup;

  rewind(src);
  while( 1 ) {
    size_t nr = fread(in_buf, 1, in_cap, src);
    ZSTD_inBuffer inb = { in_buf, nr, 0 };
    if( nr == 0 ) break;
    while( inb.pos < inb.size ) {
      ZSTD_outBuffer outb = { out_buf, out_cap, 0 };
      size_t zr = ZSTD_compressStream(zs, &outb, &inb);
      if( ZSTD_isError(zr) ) goto ZW_cleanup;
      if( outb.pos > 0 && fwrite(out_buf, 1, outb.pos, dst) != outb.pos ) goto ZW_cleanup;
    }
  }

  while( 1 ) {
    ZSTD_outBuffer outb = { out_buf, out_cap, 0 };
    size_t zr = ZSTD_endStream(zs, &outb);
    if( ZSTD_isError(zr) ) goto ZW_cleanup;
    if( outb.pos > 0 && fwrite(out_buf, 1, outb.pos, dst) != outb.pos ) goto ZW_cleanup;
    if( zr == 0 ) break;
  }

  status = 0;

ZW_cleanup:
  if( dst != NULL ) fclose(dst);
  if( zs  != NULL ) ZSTD_freeCStream(zs);
  free(in_buf);
  free(out_buf);
  return status;
#else
  return -1;
#endif
}

#ifdef HAVE_ZSTD
static int znz_get_zstd_content_size(const char *path, znz_off_t *usz)
{
  FILE *fp = NULL;
  unsigned char hbuf[18];
  size_t nread;
  unsigned long long fsz;

  if( path == NULL || usz == NULL ) return -1;
  fp = fopen(path, "rb");
  if( fp == NULL ) return -1;

  nread = fread(hbuf, 1, sizeof(hbuf), fp);
  fclose(fp);
  if( nread < 6 ) return -1;

  fsz = ZSTD_getFrameContentSize(hbuf, nread);
  if( fsz == ZSTD_CONTENTSIZE_UNKNOWN || fsz == ZSTD_CONTENTSIZE_ERROR ) return -1;

  *usz = (znz_off_t)fsz;
  return 0;
}

static int znz_init_zstd_reader(znzFile file, const char *path)
{
  char tname[64];
  ZSTD_DStream *zd = NULL;
  size_t zr;

  if( file == NULL || path == NULL ) return -1;

  if( znz_make_tmpfile(tname, sizeof(tname), &file->nzfptr) ) return -1;
  file->ztmpname = strdup(tname);
  if( file->ztmpname == NULL ) return -1;

  file->zsrcfptr = fopen(path, "rb");
  if( file->zsrcfptr == NULL ) return -1;

  zd = ZSTD_createDStream();
  if( zd == NULL ) return -1;
  zr = ZSTD_initDStream(zd);
  if( ZSTD_isError(zr) ) return -1;

  file->zstd_dstream = (void *)zd;
  file->zstin_cap = ZSTD_DStreamInSize();
  file->zstout_cap = ZSTD_DStreamOutSize();
  file->zstinbuf = (unsigned char *)malloc(file->zstin_cap);
  file->zstoutbuf = (unsigned char *)malloc(file->zstout_cap);
  if( file->zstinbuf == NULL || file->zstoutbuf == NULL ) return -1;

  file->zstin_size = 0;
  file->zstin_pos = 0;
  file->zeof = 0;
  file->zstream = 1;
  file->zpos = 0;
  file->zfilled = 0;

  if( znz_get_zstd_content_size(path, &file->zusize) == 0 ) {
    file->zsize_known = 1;
  }

  return 0;
}

static int znz_fill_zstd_to(znzFile file, znz_off_t target)
{
  ZSTD_DStream *zd;

  if( file == NULL || file->zmode != ZNZ_COMPRESS_ZSTD || file->zwrite ) return -1;
  if( file->zstream == 0 ) return 0;
  if( target <= file->zfilled || file->zeof ) return 0;

  zd = (ZSTD_DStream *)file->zstd_dstream;
  if( zd == NULL || file->zsrcfptr == NULL || file->nzfptr == NULL ) return -1;

  while( file->zfilled < target && !file->zeof ) {
    if( file->zstin_pos >= file->zstin_size ) {
      file->zstin_size = fread(file->zstinbuf, 1, file->zstin_cap, file->zsrcfptr);
      file->zstin_pos = 0;
      if( file->zstin_size == 0 ) {
        /*
         * At source EOF, zstd may still have pending output to flush
         * from previously consumed input.
         */
        ZSTD_inBuffer inb = { NULL, 0, 0 };
        while( file->zfilled < target && !file->zeof ) {
          ZSTD_outBuffer outb = { file->zstoutbuf, file->zstout_cap, 0 };
          size_t zr = ZSTD_decompressStream(zd, &outb, &inb);
          if( ZSTD_isError(zr) ) return -1;

          if( outb.pos > 0 ) {
            if( fwrite(file->zstoutbuf, 1, outb.pos, file->nzfptr) != outb.pos ) return -1;
            file->zfilled += (znz_off_t)outb.pos;
          }

          if( zr == 0 ) {
            file->zeof = 1;
            break;
          }

          /* No more compressed input and no progress: truncated stream. */
          if( outb.pos == 0 ) return -1;
        }

        if( file->zeof ) break;
        continue;
      }
    }

    {
      ZSTD_inBuffer inb = { file->zstinbuf, file->zstin_size, file->zstin_pos };
      while( inb.pos < inb.size ) {
        ZSTD_outBuffer outb = { file->zstoutbuf, file->zstout_cap, 0 };
        size_t zr = ZSTD_decompressStream(zd, &outb, &inb);
        if( ZSTD_isError(zr) ) return -1;
        if( outb.pos > 0 ) {
          if( fwrite(file->zstoutbuf, 1, outb.pos, file->nzfptr) != outb.pos ) return -1;
          file->zfilled += (znz_off_t)outb.pos;
        }
        if( zr == 0 ) {
          if( inb.pos < inb.size ) {
            if( ZSTD_isError(ZSTD_initDStream(zd)) ) return -1;
            continue;
          }
          file->zeof = 1;
          break;
        }
      }
      file->zstin_pos = inb.pos;
    }
  }

  fflush(file->nzfptr);
  return 0;
}

static int znz_fill_zstd_all(znzFile file)
{
  while( file != NULL && file->zmode == ZNZ_COMPRESS_ZSTD && !file->zwrite && !file->zeof ) {
    znz_off_t next = file->zfilled + (znz_off_t)file->zstout_cap;
    if( znz_fill_zstd_to(file, next) ) return -1;
  }
  return 0;
}

static int znz_force_zstd_decode_all(znzFile file)
{
  ZSTD_DStream *zd;

  if( file == NULL || file->zmode != ZNZ_COMPRESS_ZSTD || file->zwrite ) return -1;
  if( file->zsrcfptr == NULL || file->nzfptr == NULL || file->ztmpname == NULL ) return -1;

  if( fseek(file->zsrcfptr, 0, SEEK_SET) != 0 ) return -1;

  file->nzfptr = freopen(file->ztmpname, "wb+", file->nzfptr);
  if( file->nzfptr == NULL ) return -1;

  zd = (ZSTD_DStream *)file->zstd_dstream;
  if( zd == NULL ) return -1;
  if( ZSTD_isError(ZSTD_initDStream(zd)) ) return -1;

  file->zstin_size = 0;
  file->zstin_pos = 0;
  file->zeof = 0;
  file->zfilled = 0;
  file->zpos = 0;

  while( !file->zeof ) {
    size_t nr = fread(file->zstinbuf, 1, file->zstin_cap, file->zsrcfptr);
    ZSTD_inBuffer inb = { file->zstinbuf, nr, 0 };

    while( inb.pos < inb.size || nr == 0 ) {
      ZSTD_outBuffer outb = { file->zstoutbuf, file->zstout_cap, 0 };
      size_t zr = ZSTD_decompressStream(zd, &outb, &inb);
      if( ZSTD_isError(zr) ) return -1;

      if( outb.pos > 0 ) {
        if( fwrite(file->zstoutbuf, 1, outb.pos, file->nzfptr) != outb.pos ) return -1;
        file->zfilled += (znz_off_t)outb.pos;
      }

      if( zr == 0 ) {
        if( inb.pos < inb.size ) {
          if( ZSTD_isError(ZSTD_initDStream(zd)) ) return -1;
          continue;
        }
        file->zeof = 1;
        break;
      }

      if( nr == 0 && outb.pos == 0 ) return -1;
      if( nr > 0 && inb.pos >= inb.size ) break;
    }

    if( nr == 0 && !file->zeof ) return -1;
  }

  fflush(file->nzfptr);
  file->zsize_known = 1;
  file->zusize = file->zfilled;
  return 0;
}
#endif

/*
znzlib.c  (zipped or non-zipped library)

*****            This code is released to the public domain.            *****

*****  Author: Mark Jenkinson, FMRIB Centre, University of Oxford       *****
*****  Date:   September 2004                                           *****

*****  Neither the FMRIB Centre, the University of Oxford, nor any of   *****
*****  its employees imply any warranty of usefulness of this software  *****
*****  for any purpose, and do not assume any liability for damages,    *****
*****  incidental or otherwise, caused by any use of this document.     *****

*/


/* Note extra argument (use_compression) where
   use_compression==0 is no compression
   use_compression!=0 uses zlib (gzip) compression
*/

znzFile znzopen(const char *path, const char *mode, int use_compression)
{
  znzFile file;
  char tname[64];

  file = (znzFile) calloc(1,sizeof(struct znzptr));
  if( file == NULL ){
     fprintf(stderr,"** ERROR: znzopen failed to alloc znzptr\n");
     return NULL;
  }

  file->zmode = ZNZ_COMPRESS_NONE;
  file->zwrite = 0;
  file->zstream = 0;
  file->zeof = 0;
  file->zsize_known = 0;
  file->zpos = 0;
  file->zfilled = 0;
  file->zusize = 0;
  file->zpath = NULL;
  file->ztmpname = NULL;
  file->zsrcfptr = NULL;
  file->zstd_dstream = NULL;
  file->zstinbuf = NULL;
  file->zstoutbuf = NULL;
  file->zstin_cap = 0;
  file->zstin_size = 0;
  file->zstin_pos = 0;
  file->zstout_cap = 0;
  file->nzfptr = NULL;

  if( use_compression == ZNZ_COMPRESS_ZSTD ) {
    file->zmode = ZNZ_COMPRESS_ZSTD;
    file->zpath = strdup(path);
    if( file->zpath == NULL ) {
      free(file);
      return NULL;
    }

    if( mode && strchr(mode, 'r') ) {
      #ifdef HAVE_ZSTD
      if( znz_init_zstd_reader(file, path) ) {
        if( file->zstd_dstream != NULL ) ZSTD_freeDStream((ZSTD_DStream *)file->zstd_dstream);
        if( file->zsrcfptr != NULL ) fclose(file->zsrcfptr);
        if( file->nzfptr != NULL ) fclose(file->nzfptr);
        if( file->ztmpname != NULL ) unlink(file->ztmpname);
        free(file->ztmpname);
        free(file->zstinbuf);
        free(file->zstoutbuf);
        free(file->zpath);
        free(file);
        return NULL;
      }
      #else
      free(file->zpath);
      free(file);
      return NULL;
      #endif
    } else {
      if( znz_make_tmpfile(tname, sizeof(tname), &file->nzfptr) ) {
        free(file->zpath);
        free(file);
        return NULL;
      }

      file->ztmpname = strdup(tname);
      if( file->ztmpname == NULL ) {
        fclose(file->nzfptr);
        unlink(tname);
        free(file->zpath);
        free(file);
        return NULL;
      }

      file->zwrite = 1;
      file->zstream = 0;
      rewind(file->nzfptr);
    }

    return file;
  }

#ifdef HAVE_ZLIB
  file->zfptr = NULL;

  if (use_compression == ZNZ_COMPRESS_GZIP) {
    file->withz = 1;
    if((file->zfptr = gzopen(path,mode)) == NULL) {
        free(file);
        file = NULL;
    }
  } else {
#endif

    file->withz = 0;
    if((file->nzfptr = fopen(path,mode)) == NULL) {
      free(file);
      file = NULL;
    }

#ifdef HAVE_ZLIB
  }
#endif

  return file;
}

#ifdef COMPILE_NIFTIUNUSED_CODE
znzFile znzdopen(int fd, const char *mode, int use_compression)
{
  znzFile file;
  file = (znzFile) calloc(1,sizeof(struct znzptr));
  if( file == NULL ){
     fprintf(stderr,"** ERROR: znzdopen failed to alloc znzptr\n");
     return NULL;
  }
#ifdef HAVE_ZLIB
  if (use_compression) {
    file->withz = 1;
    file->zfptr = gzdopen(fd,mode);
    file->nzfptr = NULL;
  } else {
#endif
    file->withz = 0;
#ifdef HAVE_FDOPEN
    file->nzfptr = fdopen(fd,mode);
#endif
#ifdef HAVE_ZLIB
    file->zfptr = NULL;
  };
#endif
  return file;
}
#endif


int Xznzclose(znzFile * file)
{
  int retval = 0;
  if (*file!=NULL) {
    if( (*file)->zmode == ZNZ_COMPRESS_ZSTD ) {
      if( (*file)->zwrite ) {
        if( znz_stage_zstd_write((*file)->zpath, (*file)->nzfptr) ) retval = -1;
      }

#ifdef HAVE_ZSTD
      if( (*file)->zstd_dstream != NULL ) ZSTD_freeDStream((ZSTD_DStream *)(*file)->zstd_dstream);
#endif
      if( (*file)->zsrcfptr != NULL ) fclose((*file)->zsrcfptr);
      if( (*file)->nzfptr != NULL ) fclose((*file)->nzfptr);

      if( (*file)->ztmpname != NULL ) unlink((*file)->ztmpname);
      free((*file)->zstinbuf);
      free((*file)->zstoutbuf);
      free((*file)->ztmpname);
      free((*file)->zpath);
      free(*file);
      *file = NULL;
      return retval;
    }

#ifdef HAVE_ZLIB
    if ((*file)->zfptr!=NULL)  { retval = gzclose((*file)->zfptr); }
#endif
    if ((*file)->nzfptr!=NULL) { retval = fclose((*file)->nzfptr); }

    free(*file);
    *file = NULL;
  }
  return retval;
}


/* we already assume ints are 4 bytes */
#undef ZNZ_MAX_BLOCK_SIZE
#define ZNZ_MAX_BLOCK_SIZE (1<<30)

size_t znzread(void* buf, size_t size, size_t nmemb, znzFile file)
{
  size_t     remain = size*nmemb;
  char     * cbuf = (char *)buf;
  unsigned   n2read;
  int        nread;
  static int znzdbg_count = 0;

  if (file==NULL) { return 0; }

  if( getenv("ZNZ_DEBUG_ZSTD") && znzdbg_count < 20 ) {
    fprintf(stderr,"++ ZNZDBG: znzread size=%zu nmemb=%zu zmode=%d withz=%d zwrite=%d zstream=%d\n",
            size, nmemb, file->zmode, file->withz, file->zwrite, file->zstream);
    znzdbg_count++;
  }

  if( file->zmode == ZNZ_COMPRESS_ZSTD && !file->zwrite ) {
#ifdef HAVE_ZSTD
    size_t nobj;
    znz_off_t got;
    znz_off_t need = file->zpos + (znz_off_t)(size*nmemb);

    /* For large payload reads, avoid incremental stream corner-cases. */
    if( file->zstream && (znz_off_t)(size*nmemb) > (znz_off_t)(1<<20) ) {
      if( getenv("ZNZ_DEBUG_ZSTD") )
        fprintf(stderr,"++ ZNZDBG: large read request bytes=%lld\n", (long long)(size*nmemb));
      if( znz_force_zstd_decode_all_cmd(file) == 0 ) {
        if( fseek(file->nzfptr, file->zpos, SEEK_SET) != 0 ) return 0;
        nobj = fread(buf,size,nmemb,file->nzfptr);
        if( getenv("ZNZ_DEBUG_ZSTD") )
          fprintf(stderr,"++ ZNZDBG: large-read direct nobj=%lld nmemb=%lld\n",
                  (long long)nobj, (long long)nmemb);
        file->zpos += (znz_off_t)(nobj*size);
        return nobj;
      }
      if( getenv("ZNZ_DEBUG_ZSTD") )
        fprintf(stderr,"++ ZNZDBG: cmd-decode unavailable, falling back to stream\n");
    }

    if( znz_fill_zstd_to(file, need) ) {
      if( znz_force_zstd_decode_all(file) ) {
        if( znz_force_zstd_decode_all_cmd(file) ) return 0;
      }
    }
    if( file->zfilled < need ) {
      if( znz_force_zstd_decode_all(file) ) {
        if( znz_force_zstd_decode_all_cmd(file) ) return 0;
      }
    }
    if( fseek(file->nzfptr, file->zpos, SEEK_SET) != 0 ) return 0;
    nobj = fread(buf,size,nmemb,file->nzfptr);
    got = file->zpos + (znz_off_t)(nobj*size);
    if( nobj < nmemb && got < need ) {
      if( znz_force_zstd_decode_all(file) ) {
        if( znz_force_zstd_decode_all_cmd(file) ) return nobj;
      }
      if( fseek(file->nzfptr, file->zpos, SEEK_SET) != 0 ) return nobj;
      nobj = fread(buf,size,nmemb,file->nzfptr);
    }
    file->zpos += (znz_off_t)(nobj*size);
    return nobj;
#else
    return 0;
#endif
  }

#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) {
    /* gzread/write take unsigned int length, so maybe read in int pieces
       (noted by M Hanke, example given by M Adler)   6 July 2010 [rickr] */
    while( remain > 0 ) {
       n2read = (remain < ZNZ_MAX_BLOCK_SIZE) ? remain : ZNZ_MAX_BLOCK_SIZE;
       nread = gzread(file->zfptr, (void *)cbuf, n2read);
       if( nread < 0 ) return nread; /* returns -1 on error */

       remain -= nread;
       cbuf += nread;

       /* require reading n2read bytes, so we don't get stuck */
       if( nread < (int)n2read ) break;  /* return will be short */
    }

    /* warn of a short read that will seem complete */
    if( remain > 0 && remain < size )
       fprintf(stderr,"** znzread: read short by %u bytes\n",(unsigned)remain);

    return nmemb - remain/size;   /* return number of members processed */
  }
#endif
  return fread(buf,size,nmemb,file->nzfptr);
}

size_t znzwrite(const void* buf, size_t size, size_t nmemb, znzFile file)
{
  size_t     remain = size*nmemb;
  const char * cbuf = (const char *)buf;
  unsigned   n2write;
  int        nwritten;

  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) {
    while( remain > 0 ) {
       n2write = (remain < ZNZ_MAX_BLOCK_SIZE) ? remain : ZNZ_MAX_BLOCK_SIZE;
       nwritten = gzwrite(file->zfptr, (const void *)cbuf, n2write);

       /* gzread returns 0 on error, but in case that ever changes... */
       if( nwritten < 0 ) return nwritten;

       remain -= nwritten;
       cbuf += nwritten;

       /* require writing n2write bytes, so we don't get stuck */
       if( nwritten < (int)n2write ) break;
    }

    /* warn of a short write that will seem complete */
    if( remain > 0 && remain < size )
      fprintf(stderr,"** znzwrite: write short by %u bytes\n",(unsigned)remain);

    return nmemb - remain/size;   /* return number of members processed */
  }
#endif
  return fwrite(buf,size,nmemb,file->nzfptr);
}

znz_off_t znzseek(znzFile file, znz_off_t offset, int whence)
{
  znz_off_t target = 0;
  znz_off_t endpos = 0;

  if (file==NULL) { return 0; }

  if( file->zmode == ZNZ_COMPRESS_ZSTD ) {
    if( file->zwrite ) return fseek(file->nzfptr,offset,whence);

#ifdef HAVE_ZSTD
    if( whence == SEEK_SET ) target = offset;
    else if( whence == SEEK_CUR ) target = file->zpos + offset;
    else if( whence == SEEK_END ) {
      if( file->zsize_known ) {
        endpos = file->zusize;
      } else {
        if( znz_fill_zstd_all(file) ) return -1;
        endpos = file->zfilled;
      }
      target = endpos + offset;
    } else {
      return -1;
    }

    if( target < 0 ) return -1;
    if( target > file->zfilled ) {
      if( znz_fill_zstd_to(file, target) ) return -1;
    }
    file->zpos = target;
    return 0;
#else
    return -1;
#endif
  }

#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return (znz_off_t) gzseek(file->zfptr,offset,whence);
#endif
  return fseek(file->nzfptr,offset,whence);
}

int znzrewind(znzFile stream)
{
  if (stream==NULL) { return 0; }

  if( stream->zmode == ZNZ_COMPRESS_ZSTD ) {
    if( !stream->zwrite ) stream->zpos = 0;
    else rewind(stream->nzfptr);
    return 0;
  }

#ifdef HAVE_ZLIB
  /* On some systems, gzrewind() fails for uncompressed files.
     Use gzseek(), instead.               10, May 2005 [rickr]

     if (stream->zfptr!=NULL) return gzrewind(stream->zfptr);
  */

  if (stream->zfptr!=NULL) return (int)gzseek(stream->zfptr, 0L, SEEK_SET);
#endif
  rewind(stream->nzfptr);
  return 0;
}

znz_off_t znztell(znzFile file)
{
  if (file==NULL) { return 0; }

  if( file->zmode == ZNZ_COMPRESS_ZSTD && !file->zwrite ) return file->zpos;

#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return (znz_off_t) gztell(file->zfptr);
#endif
  return ftell(file->nzfptr);
}

int znzputs(const char * str, znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzputs(file->zfptr,str);
#endif
  return fputs(str,file->nzfptr);
}

#ifdef COMPILE_NIFTIUNUSED_CODE
char * znzgets(char* str, int size, znzFile file)
{
  if (file==NULL) { return NULL; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzgets(file->zfptr,str,size);
#endif
  return fgets(str,size,file->nzfptr);
}


int znzflush(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzflush(file->zfptr,Z_SYNC_FLUSH);
#endif
  return fflush(file->nzfptr);
}


int znzeof(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzeof(file->zfptr);
#endif
  return feof(file->nzfptr);
}


int znzputc(int c, znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzputc(file->zfptr,c);
#endif
  return fputc(c,file->nzfptr);
}


int znzgetc(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzgetc(file->zfptr);
#endif
  return fgetc(file->nzfptr);
}

#if !defined (WIN32)
int znzprintf(znzFile stream, const char *format, ...)
{
  int retval=0;
  char *tmpstr;
  va_list va;
  if (stream==NULL) { return 0; }
  va_start(va, format);
#ifdef HAVE_ZLIB
  if (stream->zfptr!=NULL) {
    int size;  /* local to HAVE_ZLIB block */
    size = strlen(format) + 1000000;  /* overkill I hope */
    tmpstr = (char *)calloc(1, size);
    if( tmpstr == NULL ){
       fprintf(stderr,"** ERROR: znzprintf failed to alloc %d bytes\n", size);
       return retval;
    }
    vsprintf(tmpstr,format,va);
    retval=gzprintf(stream->zfptr,"%s",tmpstr);
    free(tmpstr);
  } else
#endif
  {
   retval=vfprintf(stream->nzfptr,format,va);
  }
  va_end(va);
  return retval;
}
#endif

#endif
