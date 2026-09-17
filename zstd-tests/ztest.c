/* znzlib zstd stress test: ztest write|check|bench <file.zst> <ref> */
#include "znzlib.h"
#include <time.h>

static unsigned char *slurp(const char *fn, size_t *n)
{
  FILE *fp = fopen(fn, "rb"); unsigned char *b;
  if( !fp ) { perror(fn); exit(2); }
  fseek(fp, 0, SEEK_END); *n = ftell(fp); rewind(fp);
  b = malloc(*n ? *n : 1);
  if( fread(b, 1, *n, fp) != *n ) exit(2);
  fclose(fp); return b;
}

static double now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }

int main(int argc, char **argv)
{
  size_t n, off, k; unsigned char *ref, *got; znzFile zf; int ii, bad = 0;
  if( argc < 4 ) return 1;
  ref = slurp(argv[3], &n);
  srand(12345);

  if( !strcmp(argv[1], "write") ) {
    double t0 = now();
    zf = znzopen(argv[2], "wb", ZNZ_COMPRESS_ZSTD);
    if( !zf ) { fprintf(stderr, "open failed\n"); return 1; }
    for( off = 0; off < n; off += k ) {        /* ragged write sizes */
      k = (size_t)(rand() % 3000000) + 1;
      if( rand() % 4 == 0 ) k = rand() % 7 + 1;
      if( k > n - off ) k = n - off;
      if( znzwrite(ref+off, 1, k, zf) != k ) { fprintf(stderr, "short write\n"); return 1; }
    }
    if( znzclose(zf) ) { fprintf(stderr, "close failed\n"); return 1; }
    free(ref);
    printf("write %.1f MB/s\n", n/1e6/(now()-t0));
    return 0;
  }

  got = malloc(n + 100);

  if( !strcmp(argv[1], "bench") ) {           /* sequential 4MB "bricks" */
    double t0, t1;
    memset(got, 1, n);                         /* prefault the output */
    t0 = now();
    zf = znzopen(argv[2], "rb", ZNZ_COMPRESS_ZSTD);
    for( off = 0; off < n; off += k ) {
      k = znzread(got+off, 1, 4<<20, zf);
      if( k == 0 ) break;
    }
    znzclose(zf);
    t1 = now();
    printf("read %.1f MB/s %s\n", n/1e6/(t1-t0), memcmp(got,ref,n) ? "MISMATCH" : "ok");
    return 0;
  }

  /* check: header read, full read, random seeks, EOF, SEEK_END */
  zf = znzopen(argv[2], "rb", ZNZ_COMPRESS_ZSTD);
  if( !zf ) { fprintf(stderr, "open failed\n"); return 1; }
  k = znzread(got, 1, 348, zf);
  if( k != (n < 348 ? n : 348) || memcmp(got, ref, k) ) { printf("header FAIL\n"); bad++; }
  znzseek(zf, 0, SEEK_SET);
  k = znzread(got, 1, n + 50, zf);
  if( k != n || memcmp(got, ref, n) ) { printf("full read FAIL (%zu of %zu)\n", k, n); bad++; }
  if( znzread(got, 1, 10, zf) != 0 ) { printf("EOF FAIL\n"); bad++; }
  if( znztell(zf) != (znz_off_t)n ) { printf("tell FAIL\n"); bad++; }
  for( ii = 0; ii < 300 && n > 0; ii++ ) {
    size_t o = (size_t)(((double)rand()/RAND_MAX) * (n-1)), want;
    k = (size_t)(rand() % 5000000);
    want = (o + k > n) ? n - o : k;
    if( znzseek(zf, (znz_off_t)o, SEEK_SET) ) { printf("seek FAIL\n"); bad++; break; }
    if( znzread(got, 1, k, zf) != want || memcmp(got, ref+o, want) ) {
      printf("random read FAIL at %zu len %zu\n", o, k); bad++; break;
    }
    if( rand() % 2 ) {   /* relative seek and read again */
      znzseek(zf, -(znz_off_t)(want/2), SEEK_CUR);
      o += want - want/2;
      if( znztell(zf) != (znz_off_t)o ) { printf("SEEK_CUR FAIL\n"); bad++; break; }
    }
  }
  if( znzseek(zf, -10, SEEK_END) == 0 && n >= 10 ) {
    if( znzread(got, 1, 10, zf) != 10 || memcmp(got, ref+n-10, 10) ) { printf("SEEK_END FAIL\n"); bad++; }
  }
  znzclose(zf);
  free(ref); free(got);
  printf("%s %s\n", bad ? "FAILED" : "PASS", argv[2]);
  return bad != 0;
}
