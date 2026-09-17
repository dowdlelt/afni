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

/*----------------------------------------------------------------------
  zstd support (HAVE_ZSTD)

  Files are written as a series of independent zstd frames, each preceded
  by a 12-byte skippable frame holding the compressed size of the frame
  that follows.  This is the layout used by pzstd, so the files decompress
  with 'zstd -d' (skippable frames are ignored) and in parallel with
  'pzstd -d'.  The first frame is kept small, so reading a header only
  decompresses a little data.

  Reading an indexed file (ours or pzstd's) walks the index lazily and
  decompresses frames on demand, reading ahead in parallel when access is
  sequential.  Any other zstd file (e.g. from the zstd CLI) is decoded as
  a single stream.

  Environment controls:
     AFNI_ZSTD_LEVEL     compression level          (default 3)
     AFNI_ZSTD_THREADS   worker threads             (default #CPUs, max 16)
     AFNI_ZSTD_FRAME_MB  uncompressed MB per frame  (default 8)
  ----------------------------------------------------------------------*/

#ifdef HAVE_ZSTD

#include <zstd.h>

#if ZSTD_VERSION_NUMBER < 10400
#error "znzlib zstd support requires libzstd >= 1.4.0"
#endif

/* threads are used for parallel (un)compression of frames; define
   ZNZ_ZSTD_NO_THREADS to build the single-threaded version */
#if !defined(_WIN32) && !defined(_WIN64) && !defined(ZNZ_ZSTD_NO_THREADS)
#include <pthread.h>
#include <unistd.h>
#define ZNZ_ZSTD_PTHREADS
#endif

#define ZNZ_SKIP_MAGIC      0x184D2A50U  /* pzstd skippable frame magic   */
#define ZNZ_SKIP_HSIZE      12           /* magic + size(4) + frame csize */
#define ZNZ_FIRST_FRAME     ((size_t)1<<20)
#define ZNZ_MAX_THREADS     16

typedef struct {
  znz_off_t       coff;    /* file offset of the zstd frame           */
  size_t          csize;   /* compressed size of the frame            */
  znz_off_t       uoff;    /* uncompressed offset (valid if uknown)   */
  size_t          usize;   /* uncompressed size   (valid if uknown)   */
  int             uknown;
  unsigned char * data;    /* decoded frame, if cached                */
} znz_zframe;

struct znz_zstd {
  char       * path;       /* for messages */
  int          writing;
  int          level, nthreads;
  size_t       frame_size;
  znz_off_t    pos;        /* current uncompressed position */

  /* reading, indexed */
  int          indexed, index_done;
  znz_zframe * frames;
  int          nframes, frames_alloc;
  int          hint;       /* frame of last access */
  int          ra_end, ra; /* readahead: end of last batch, batch size */

  /* reading, single stream */
  ZSTD_DCtx     * ds;
  unsigned char * inbuf, * outbuf;
  size_t          in_cap, in_size, in_pos, out_cap, out_len;
  znz_off_t       out_start;
  int             at_frame_end, stream_end;

  /* per-thread contexts */
  ZSTD_CCtx ** cctx;
  ZSTD_DCtx ** dctx;

  /* writing */
  unsigned char ** wbuf, ** cbuf;
  size_t         * wlen, * ccap;
  int              wcur, nwritten;
};

static int znz_env_int(const char *name, int def, int lo, int hi)
{
  char *e = getenv(name), *end;
  long  v;
  if( e == NULL || *e == '\0' ) return def;
  v = strtol(e, &end, 10);
  if( end == e ) return def;
  if( v < lo ) v = lo;
  if( v > hi ) v = hi;
  return (int)v;
}

static void znz_put_le32(unsigned char *p, unsigned v)
{
  p[0] = v & 0xff; p[1] = (v>>8) & 0xff; p[2] = (v>>16) & 0xff; p[3] = v>>24;
}

static unsigned znz_get_le32(const unsigned char *p)
{
  return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16)
                        | ((unsigned)p[3]<<24);
}

/* run njobs calls of fn, one thread per job (inline if only one) */
static void znz_run_jobs(void *(*fn)(void *), void *jobs, size_t jsize,
                         int njobs)
{
  int ii;
#ifdef ZNZ_ZSTD_PTHREADS
  pthread_t tid[ZNZ_MAX_THREADS];
  int       started[ZNZ_MAX_THREADS];

  if( njobs > 1 && njobs <= ZNZ_MAX_THREADS ) {
    for( ii = 1; ii < njobs; ii++ )
      started[ii] = pthread_create(&tid[ii], NULL, fn,
                                   (char *)jobs + ii*jsize) == 0;
    fn(jobs);
    for( ii = 1; ii < njobs; ii++ ) {
      if( started[ii] ) pthread_join(tid[ii], NULL);
      else              fn((char *)jobs + ii*jsize);
    }
    return;
  }
#endif
  for( ii = 0; ii < njobs; ii++ ) fn((char *)jobs + ii*jsize);
}

static struct znz_zstd * znz_zstd_new(const char *path, int writing)
{
  struct znz_zstd *z = (struct znz_zstd *)calloc(1, sizeof(*z));
  int ncpu = 1;

  if( z == NULL ) return NULL;
  z->path = (char *)malloc(strlen(path)+1);
  if( z->path == NULL ) { free(z); return NULL; }
  strcpy(z->path, path);
  z->writing = writing;

#ifdef ZNZ_ZSTD_PTHREADS
  ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if( ncpu < 1 ) ncpu = 1;
  if( ncpu > ZNZ_MAX_THREADS ) ncpu = ZNZ_MAX_THREADS;
#endif
  z->nthreads   = znz_env_int("AFNI_ZSTD_THREADS", ncpu, 1, ZNZ_MAX_THREADS);
  z->level      = znz_env_int("AFNI_ZSTD_LEVEL", 3, -7, 22);
  z->frame_size = (size_t)znz_env_int("AFNI_ZSTD_FRAME_MB", 8, 1, 1024) << 20;

  z->cctx = (ZSTD_CCtx **)calloc(z->nthreads, sizeof(ZSTD_CCtx *));
  z->dctx = (ZSTD_DCtx **)calloc(z->nthreads, sizeof(ZSTD_DCtx *));
  z->wbuf = (unsigned char **)calloc(z->nthreads, sizeof(unsigned char *));
  z->cbuf = (unsigned char **)calloc(z->nthreads, sizeof(unsigned char *));
  z->wlen = (size_t *)calloc(z->nthreads, sizeof(size_t));
  z->ccap = (size_t *)calloc(z->nthreads, sizeof(size_t));
  if( !z->cctx || !z->dctx || !z->wbuf || !z->cbuf || !z->wlen || !z->ccap ) {
    free(z->cctx); free(z->dctx); free(z->wbuf); free(z->cbuf);
    free(z->wlen); free(z->ccap); free(z->path); free(z);
    return NULL;
  }
  return z;
}

static void znz_zstd_free(struct znz_zstd *z)
{
  int ii;
  if( z == NULL ) return;
  for( ii = 0; ii < z->nframes; ii++ ) free(z->frames[ii].data);
  for( ii = 0; ii < z->nthreads; ii++ ) {
    ZSTD_freeCCtx(z->cctx[ii]);
    ZSTD_freeDCtx(z->dctx[ii]);
    free(z->wbuf[ii]);
    free(z->cbuf[ii]);
  }
  ZSTD_freeDCtx(z->ds);
  free(z->frames); free(z->inbuf); free(z->outbuf);
  free(z->cctx); free(z->dctx); free(z->wbuf); free(z->cbuf);
  free(z->wlen); free(z->ccap); free(z->path);
  free(z);
}

/*--------------------------- writing ---------------------------------*/

typedef struct {
  ZSTD_CCtx     * cctx;
  const unsigned char * src;
  size_t          srclen;
  unsigned char * dst;
  size_t          dstcap, result;
  int             level;
} znz_cjob;

static void * znz_cjob_run(void *arg)
{
  znz_cjob *j = (znz_cjob *)arg;
  ZSTD_CCtx_reset(j->cctx, ZSTD_reset_session_and_parameters);
  ZSTD_CCtx_setParameter(j->cctx, ZSTD_c_compressionLevel, j->level);
  ZSTD_CCtx_setParameter(j->cctx, ZSTD_c_checksumFlag, 1);
  j->result = ZSTD_compress2(j->cctx, j->dst, j->dstcap, j->src, j->srclen);
  return NULL;
}

/* compress and write the first nbuf write buffers, in parallel */
static int znz_zstd_flush(struct znz_zstd *z, FILE *fp, int nbuf)
{
  znz_cjob       jobs[ZNZ_MAX_THREADS];
  unsigned char  hdr[ZNZ_SKIP_HSIZE];
  int            ii, rv = 0;

  for( ii = 0; ii < nbuf; ii++ ) {
    size_t need = ZSTD_compressBound(z->wlen[ii]);
    if( z->cctx[ii] == NULL && (z->cctx[ii] = ZSTD_createCCtx()) == NULL )
      return -1;
    if( z->ccap[ii] < need ) {
      free(z->cbuf[ii]);
      z->cbuf[ii] = (unsigned char *)malloc(need);
      z->ccap[ii] = z->cbuf[ii] ? need : 0;
      if( z->cbuf[ii] == NULL ) return -1;
    }
    jobs[ii].cctx   = z->cctx[ii];
    jobs[ii].src    = z->wbuf[ii];
    jobs[ii].srclen = z->wlen[ii];
    jobs[ii].dst    = z->cbuf[ii];
    jobs[ii].dstcap = z->ccap[ii];
    jobs[ii].level  = z->level;
  }

  znz_run_jobs(znz_cjob_run, jobs, sizeof(znz_cjob), nbuf);

  for( ii = 0; ii < nbuf; ii++ ) {
    if( ZSTD_isError(jobs[ii].result) ) {
      fprintf(stderr, "** znzlib: zstd compression failed for %s: %s\n",
              z->path, ZSTD_getErrorName(jobs[ii].result));
      rv = -1; break;
    }
    znz_put_le32(hdr,   ZNZ_SKIP_MAGIC);
    znz_put_le32(hdr+4, 4);
    znz_put_le32(hdr+8, (unsigned)jobs[ii].result);
    if( fwrite(hdr, 1, ZNZ_SKIP_HSIZE, fp) != ZNZ_SKIP_HSIZE ||
        fwrite(z->cbuf[ii], 1, jobs[ii].result, fp) != jobs[ii].result ) {
      fprintf(stderr, "** znzlib: failed to write %s\n", z->path);
      rv = -1; break;
    }
    z->wlen[ii] = 0;
    z->nwritten++;
  }
  z->wcur = 0;
  return rv;
}

/* capacity of write buffer slot ii: the very first frame is small */
static size_t znz_zstd_slot_cap(const struct znz_zstd *z, int ii)
{
  if( z->nwritten == 0 && ii == 0 && z->frame_size > ZNZ_FIRST_FRAME )
    return ZNZ_FIRST_FRAME;
  return z->frame_size;
}

static size_t znz_zstd_write(struct znz_zstd *z, FILE *fp,
                             const unsigned char *buf, size_t nbytes)
{
  size_t done = 0;

  while( done < nbytes ) {
    size_t cap = znz_zstd_slot_cap(z, z->wcur), ncopy;
    if( z->wbuf[z->wcur] == NULL &&
        (z->wbuf[z->wcur] = (unsigned char *)malloc(z->frame_size)) == NULL )
      break;
    ncopy = cap - z->wlen[z->wcur];
    if( ncopy > nbytes - done ) ncopy = nbytes - done;
    memcpy(z->wbuf[z->wcur] + z->wlen[z->wcur], buf + done, ncopy);
    z->wlen[z->wcur] += ncopy;
    done   += ncopy;
    z->pos += (znz_off_t)ncopy;

    if( z->wlen[z->wcur] == cap ) {
      if( ++z->wcur == z->nthreads && znz_zstd_flush(z, fp, z->nthreads) )
        break;
    }
  }
  return done;
}

static int znz_zstd_close_write(struct znz_zstd *z, FILE *fp)
{
  int nbuf = z->wcur + (z->wlen[z->wcur] > 0 ? 1 : 0);

  /* an empty file still gets one (empty) frame */
  if( nbuf == 0 && z->nwritten == 0 ) {
    if( z->wbuf[0] == NULL && (z->wbuf[0] = (unsigned char *)malloc(1)) == NULL )
      return -1;
    nbuf = 1;
  }
  return nbuf > 0 ? znz_zstd_flush(z, fp, nbuf) : 0;
}

/*--------------------------- reading ---------------------------------*/

/* add the next frame to the index: return 0 on success, 1 at EOF, -1 on error */
static int znz_zstd_extend_index(struct znz_zstd *z, FILE *fp)
{
  unsigned char hdr[ZNZ_SKIP_HSIZE];
  znz_off_t     off = 0;
  size_t        nread;
  znz_zframe  * fr;

  if( z->index_done ) return 1;
  if( z->nframes > 0 )
    off = z->frames[z->nframes-1].coff + (znz_off_t)z->frames[z->nframes-1].csize;

  if( fseek(fp, off, SEEK_SET) ) return -1;
  nread = fread(hdr, 1, ZNZ_SKIP_HSIZE, fp);
  if( nread == 0 && feof(fp) && z->nframes > 0 ) { z->index_done = 1; return 1; }
  if( nread != ZNZ_SKIP_HSIZE || znz_get_le32(hdr) != ZNZ_SKIP_MAGIC ||
      znz_get_le32(hdr+4) != 4 ) {
    fprintf(stderr, "** znzlib: bad zstd frame index in %s at offset %lld\n",
            z->path, (long long)off);
    return -1;
  }

  if( z->nframes == z->frames_alloc ) {
    int nalloc = z->frames_alloc ? 2*z->frames_alloc : 64;
    fr = (znz_zframe *)realloc(z->frames, nalloc*sizeof(znz_zframe));
    if( fr == NULL ) return -1;
    z->frames = fr; z->frames_alloc = nalloc;
  }
  fr = z->frames + z->nframes;
  memset(fr, 0, sizeof(*fr));
  fr->coff  = off + ZNZ_SKIP_HSIZE;
  fr->csize = znz_get_le32(hdr+8);
  z->nframes++;
  return 0;
}

typedef struct {
  ZSTD_DCtx     * dctx;
  int             fd;       /* read the frame with pread, if >= 0 */
  znz_off_t       coff;
  unsigned char * src;
  size_t          srclen;
  unsigned char * dst;
  size_t          usize;
  int             err, skip;
} znz_djob;

static void * znz_djob_run(void *arg)
{
  znz_djob *j = (znz_djob *)arg;
  unsigned long long fcs;

  if( j->skip ) return NULL;
  j->err = 1;
#ifdef ZNZ_ZSTD_PTHREADS
  if( j->fd >= 0 ) {
    size_t  got = 0;
    ssize_t nr;
    while( got < j->srclen ) {
      nr = pread(j->fd, j->src + got, j->srclen - got, (off_t)(j->coff + got));
      if( nr <= 0 ) return NULL;
      got += (size_t)nr;
    }
  }
#endif
  fcs = ZSTD_getFrameContentSize(j->src, j->srclen);
  if( fcs == ZSTD_CONTENTSIZE_ERROR ) return NULL;

  if( fcs != ZSTD_CONTENTSIZE_UNKNOWN ) {   /* size is in the frame header */
    size_t r;
    j->dst = (unsigned char *)malloc(fcs ? (size_t)fcs : 1);
    if( j->dst == NULL ) return NULL;
    r = ZSTD_decompressDCtx(j->dctx, j->dst, (size_t)fcs, j->src, j->srclen);
    if( ZSTD_isError(r) || r != fcs ) return NULL;
    j->usize = (size_t)fcs;
  } else {                                  /* e.g. pzstd: grow as needed  */
    ZSTD_inBuffer  in  = { j->src, j->srclen, 0 };
    ZSTD_outBuffer out = { NULL, 0, 0 };
    size_t r = 1;
    out.size = j->srclen < ((size_t)1<<20) ? ((size_t)4<<20) : 4*j->srclen;
    out.dst  = j->dst = (unsigned char *)malloc(out.size);
    ZSTD_DCtx_reset(j->dctx, ZSTD_reset_session_only);
    while( j->dst != NULL ) {
      r = ZSTD_decompressStream(j->dctx, &out, &in);
      if( ZSTD_isError(r) || r == 0 ) break;
      if( out.pos == out.size ) {
        unsigned char *nd = (unsigned char *)realloc(j->dst, 2*out.size);
        if( nd == NULL ) break;
        out.dst = j->dst = nd; out.size *= 2;
      } else if( in.pos == in.size ) {
        break;                              /* truncated frame */
      }
    }
    if( j->dst == NULL || r != 0 || in.pos != in.size ) return NULL;
    j->usize = out.pos;
  }
  j->err = 0;
  return NULL;
}

/* decode up to z->ra frames starting at frame i, in parallel */
static int znz_zstd_decode_batch(struct znz_zstd *z, FILE *fp, int i)
{
  znz_djob jobs[ZNZ_MAX_THREADS];
  int      nb, ii, rv = 0;

  /* sequential access doubles the readahead; anything else resets it */
  z->ra = (i == z->ra_end && z->ra > 0) ? 2*z->ra : 1;
  if( z->ra > z->nthreads ) z->ra = z->nthreads;

  for( nb = 0; nb < z->ra; nb++ ) {
    if( i+nb >= z->nframes ) {
      int ev = znz_zstd_extend_index(z, fp);
      if( ev < 0 ) return -1;
      if( ev > 0 ) break;
    }
  }
  z->ra_end = i + nb;

  /* evict decoded frames outside of this batch */
  for( ii = 0; ii < z->nframes; ii++ )
    if( z->frames[ii].data && (ii < i || ii >= i+nb) ) {
      free(z->frames[ii].data); z->frames[ii].data = NULL;
    }

  /* set up jobs, using the per-thread cbuf for compressed input */
  for( ii = 0; ii < nb; ii++ ) {
    znz_zframe *fr = z->frames + i + ii;
    memset(jobs+ii, 0, sizeof(znz_djob));
    if( fr->data != NULL ) { jobs[ii].skip = 1; continue; }   /* cached */
    if( z->dctx[ii] == NULL && (z->dctx[ii] = ZSTD_createDCtx()) == NULL ) {
      rv = -1; break;
    }
    jobs[ii].dctx   = z->dctx[ii];
    if( z->ccap[ii] < fr->csize ) {
      free(z->cbuf[ii]);
      z->cbuf[ii] = (unsigned char *)malloc(fr->csize);
      z->ccap[ii] = z->cbuf[ii] ? fr->csize : 0;
    }
    jobs[ii].src    = z->cbuf[ii];
    jobs[ii].srclen = fr->csize;
    jobs[ii].coff   = fr->coff;
#ifdef ZNZ_ZSTD_PTHREADS
    jobs[ii].fd     = fileno(fp);   /* read in the worker thread */
#else
    jobs[ii].fd     = -1;
    if( jobs[ii].src && (fseek(fp, fr->coff, SEEK_SET) ||
        fread(jobs[ii].src, 1, fr->csize, fp) != fr->csize) ) jobs[ii].src = NULL;
#endif
    if( jobs[ii].src == NULL && fr->csize > 0 ) {
      fprintf(stderr, "** znzlib: failed to read zstd frame %d of %s\n",
              i+ii, z->path);
      rv = -1; break;
    }
  }
  nb = ii;

  if( rv == 0 ) znz_run_jobs(znz_djob_run, jobs, sizeof(znz_djob), nb);

  for( ii = 0; ii < nb; ii++ ) {
    znz_zframe *fr = z->frames + i + ii;
    if( jobs[ii].skip ) continue;
    if( rv == 0 && jobs[ii].err ) {
      fprintf(stderr, "** znzlib: failed to decompress zstd frame %d of %s\n",
              i+ii, z->path);
      rv = -1;
    }
    if( rv == 0 ) {
      fr->data = jobs[ii].dst; fr->usize = jobs[ii].usize; fr->uknown = 1;
      if( i+ii == 0 ) fr->uoff = 0;
      else if( fr[-1].uknown ) fr->uoff = fr[-1].uoff + (znz_off_t)fr[-1].usize;
    } else {
      free(jobs[ii].dst);
    }
  }
  return rv;
}

/* return the frame holding position pos, z->nframes at EOF, -1 on error */
static int znz_zstd_locate(struct znz_zstd *z, FILE *fp, znz_off_t pos)
{
  int ii = 0;

  if( z->hint < z->nframes && z->frames[z->hint].uknown &&
      z->frames[z->hint].uoff <= pos )
    ii = z->hint;

  for( ; ; ii++ ) {
    if( ii == z->nframes ) {
      int ev = znz_zstd_extend_index(z, fp);
      if( ev < 0 ) return -1;
      if( ev > 0 ) return z->nframes;
    }
    /* note: extend_index and decode_batch may realloc z->frames, so the
       frame is always addressed through z->frames, never a saved pointer */
    if( !z->frames[ii].uknown && znz_zstd_decode_batch(z, fp, ii) ) return -1;
    if( pos < z->frames[ii].uoff + (znz_off_t)z->frames[ii].usize ) {
      z->hint = ii; return ii;
    }
  }
}

static size_t znz_zstd_read_indexed(struct znz_zstd *z, FILE *fp,
                                    unsigned char *buf, size_t nbytes)
{
  size_t done = 0;

  while( done < nbytes ) {
    int         ii = znz_zstd_locate(z, fp, z->pos);
    size_t      off, ncopy;

    if( ii < 0 || ii >= z->nframes ) break;
    if( z->frames[ii].data == NULL && znz_zstd_decode_batch(z, fp, ii) ) break;

    off   = (size_t)(z->pos - z->frames[ii].uoff);
    ncopy = z->frames[ii].usize - off;
    if( ncopy > nbytes - done ) ncopy = nbytes - done;
    memcpy(buf + done, z->frames[ii].data + off, ncopy);
    done   += ncopy;
    z->pos += (znz_off_t)ncopy;
  }
  return done;
}

static int znz_zstd_stream_restart(struct znz_zstd *z, FILE *fp)
{
  if( fseek(fp, 0, SEEK_SET) ) return -1;
  ZSTD_DCtx_reset(z->ds, ZSTD_reset_session_only);
  z->in_size = z->in_pos = z->out_len = 0;
  z->out_start = 0;
  z->at_frame_end = 1;
  z->stream_end = 0;
  return 0;
}

/* decode the next chunk of a single stream: 0 on success, 1 at end, -1 error */
static int znz_zstd_stream_next(struct znz_zstd *z, FILE *fp)
{
  z->out_start += (znz_off_t)z->out_len;
  z->out_len = 0;

  while( z->out_len == 0 ) {
    ZSTD_inBuffer  in;
    ZSTD_outBuffer out = { z->outbuf, z->out_cap, 0 };
    size_t r;

    if( z->in_pos == z->in_size ) {
      z->in_size = fread(z->inbuf, 1, z->in_cap, fp);
      z->in_pos  = 0;
      if( z->in_size == 0 ) {
        if( !z->at_frame_end ) {
          fprintf(stderr, "** znzlib: truncated zstd file %s\n", z->path);
          return -1;
        }
        z->stream_end = 1;
        return 1;
      }
    }
    in.src = z->inbuf; in.size = z->in_size; in.pos = z->in_pos;
    r = ZSTD_decompressStream(z->ds, &out, &in);
    if( ZSTD_isError(r) ) {
      fprintf(stderr, "** znzlib: zstd decompression failed for %s: %s\n",
              z->path, ZSTD_getErrorName(r));
      return -1;
    }
    z->in_pos       = in.pos;
    z->out_len      = out.pos;
    z->at_frame_end = (r == 0);
  }
  return 0;
}

static size_t znz_zstd_read_stream(struct znz_zstd *z, FILE *fp,
                                   unsigned char *buf, size_t nbytes)
{
  size_t done = 0;

  while( done < nbytes ) {
    if( z->pos < z->out_start && znz_zstd_stream_restart(z, fp) ) break;

    if( z->pos < z->out_start + (znz_off_t)z->out_len ) {
      size_t off   = (size_t)(z->pos - z->out_start);
      size_t ncopy = z->out_len - off;
      if( ncopy > nbytes - done ) ncopy = nbytes - done;
      memcpy(buf + done, z->outbuf + off, ncopy);
      done   += ncopy;
      z->pos += (znz_off_t)ncopy;
    } else if( z->stream_end || znz_zstd_stream_next(z, fp) ) {
      break;
    }
  }
  return done;
}

/* open the zstd state for a file already opened as fp */
static struct znz_zstd * znz_zstd_open(const char *path, const char *mode,
                                       FILE *fp)
{
  struct znz_zstd *z = znz_zstd_new(path, strchr(mode,'r') == NULL);
  unsigned char    hdr[ZNZ_SKIP_HSIZE];

  if( z == NULL || z->writing ) return z;

  /* indexed (pzstd-style) if the file starts with a frame index entry */
  if( fread(hdr, 1, ZNZ_SKIP_HSIZE, fp) == ZNZ_SKIP_HSIZE &&
      znz_get_le32(hdr) == ZNZ_SKIP_MAGIC && znz_get_le32(hdr+4) == 4 ) {
    z->indexed = 1;
    return z;
  }

  z->ds      = ZSTD_createDCtx();
  z->in_cap  = ZSTD_DStreamInSize();
  z->out_cap = ZSTD_DStreamOutSize();
  z->inbuf   = (unsigned char *)malloc(z->in_cap);
  z->outbuf  = (unsigned char *)malloc(z->out_cap);
  if( z->ds == NULL || z->inbuf == NULL || z->outbuf == NULL ||
      znz_zstd_stream_restart(z, fp) ) {
    znz_zstd_free(z);
    return NULL;
  }
  return z;
}

static znz_off_t znz_zstd_seek(struct znz_zstd *z, FILE *fp,
                               znz_off_t offset, int whence)
{
  znz_off_t target;

  if( whence == SEEK_SET )      target = offset;
  else if( whence == SEEK_CUR ) target = z->pos + offset;
  else if( whence == SEEK_END && z->indexed && !z->writing ) {
    /* locate a position past any real data, to index and size every frame */
    znz_off_t past_end = (znz_off_t)1 << (8*sizeof(znz_off_t) - 2);
    int ii = znz_zstd_locate(z, fp, past_end);
    if( ii != z->nframes || z->nframes == 0 ) return -1;
    target = z->frames[ii-1].uoff + (znz_off_t)z->frames[ii-1].usize + offset;
  }
  else return -1;

  if( target < 0 ) return -1;

  if( z->writing ) {   /* only forward, padding with zeros (like gzseek) */
    unsigned char zeros[4096];
    if( target < z->pos ) return -1;
    memset(zeros, 0, sizeof(zeros));
    while( z->pos < target ) {
      size_t n = (target - z->pos) < (znz_off_t)sizeof(zeros) ?
                 (size_t)(target - z->pos) : sizeof(zeros);
      if( znz_zstd_write(z, fp, zeros, n) != n ) return -1;
    }
  }
  z->pos = target;
  return 0;
}

#endif  /* HAVE_ZSTD */

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
  file = (znzFile) calloc(1,sizeof(struct znzptr));
  if( file == NULL ){
     fprintf(stderr,"** ERROR: znzopen failed to alloc znzptr\n");
     return NULL;
  }

  file->nzfptr = NULL;

  if (use_compression == ZNZ_COMPRESS_ZSTD) {
#ifdef HAVE_ZSTD
    if((file->nzfptr = fopen(path,mode)) == NULL ||
       (file->zst = znz_zstd_open(path,mode,file->nzfptr)) == NULL) {
      if( file->nzfptr != NULL ) fclose(file->nzfptr);
      free(file);
      file = NULL;
    }
#else
    fprintf(stderr,"** ERROR: znzopen: no zstd support for '%s'\n", path);
    free(file);
    file = NULL;
#endif
    return file;
  }

#ifdef HAVE_ZLIB
  file->zfptr = NULL;

  if (use_compression) {
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
#ifdef HAVE_ZSTD
    if ((*file)->zst!=NULL) {
      if ((*file)->zst->writing) retval = znz_zstd_close_write((*file)->zst, (*file)->nzfptr);
      znz_zstd_free((*file)->zst);
    }
#endif
#ifdef HAVE_ZLIB
    if ((*file)->zfptr!=NULL)  { retval = gzclose((*file)->zfptr); }
#endif
    if ((*file)->nzfptr!=NULL) { if (fclose((*file)->nzfptr)) retval = -1; }

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

  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    if (file->zst->writing || size == 0) return 0;
    if (file->zst->indexed)
      remain = znz_zstd_read_indexed(file->zst, file->nzfptr, (unsigned char *)buf, remain);
    else
      remain = znz_zstd_read_stream(file->zst, file->nzfptr, (unsigned char *)buf, remain);
    return remain/size;
  }
#endif
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
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    if (!file->zst->writing || size == 0) return 0;
    return znz_zstd_write(file->zst, file->nzfptr, (const unsigned char *)buf, remain)/size;
  }
#endif
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
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) return znz_zstd_seek(file->zst,file->nzfptr,offset,whence);
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return (znz_off_t) gzseek(file->zfptr,offset,whence);
#endif
  return fseek(file->nzfptr,offset,whence);
}

int znzrewind(znzFile stream)
{
  if (stream==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (stream->zst!=NULL) return (int)znz_zstd_seek(stream->zst,stream->nzfptr,0,SEEK_SET);
#endif
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
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) return file->zst->pos;
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return (znz_off_t) gztell(file->zfptr);
#endif
  return ftell(file->nzfptr);
}

int znzputs(const char * str, znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) return (int)znzwrite(str,1,strlen(str),file);
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzputs(file->zfptr,str);
#endif
  return fputs(str,file->nzfptr);
}

#ifdef COMPILE_NIFTIUNUSED_CODE
char * znzgets(char* str, int size, znzFile file)
{
  if (file==NULL) { return NULL; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    int ii = 0;
    while (ii < size-1 && znzread(str+ii,1,1,file) == 1)
      if (str[ii++] == '\n') break;
    if (ii == 0 || size < 1) return NULL;
    str[ii] = '\0';
    return str;
  }
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzgets(file->zfptr,str,size);
#endif
  return fgets(str,size,file->nzfptr);
}


int znzflush(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) return 0;  /* frames are written as they fill */
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzflush(file->zfptr,Z_SYNC_FLUSH);
#endif
  return fflush(file->nzfptr);
}


int znzeof(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    struct znz_zstd *z = file->zst;
    if (z->writing) return 0;
    if (z->indexed) return znz_zstd_locate(z,file->nzfptr,z->pos) == z->nframes;
    return z->stream_end && z->pos >= z->out_start + (znz_off_t)z->out_len;
  }
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzeof(file->zfptr);
#endif
  return feof(file->nzfptr);
}


int znzputc(int c, znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    unsigned char ch = (unsigned char)c;
    return znzwrite(&ch,1,1,file) == 1 ? ch : EOF;
  }
#endif
#ifdef HAVE_ZLIB
  if (file->zfptr!=NULL) return gzputc(file->zfptr,c);
#endif
  return fputc(c,file->nzfptr);
}


int znzgetc(znzFile file)
{
  if (file==NULL) { return 0; }
#ifdef HAVE_ZSTD
  if (file->zst!=NULL) {
    unsigned char ch;
    return znzread(&ch,1,1,file) == 1 ? ch : EOF;
  }
#endif
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
#ifdef HAVE_ZSTD
  if (stream->zst!=NULL) {
    int size = vsnprintf(NULL, 0, format, va);
    va_end(va);
    if (size < 0) return 0;
    tmpstr = (char *)malloc(size + 1);
    if (tmpstr == NULL) return 0;
    va_start(va, format);
    vsnprintf(tmpstr, size + 1, format, va);
    retval = (int)znzwrite(tmpstr, 1, size, stream);
    free(tmpstr);
  } else
#endif
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
