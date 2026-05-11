#include <stdio.h>
#include <stdlib.h>
#ifdef sign_is_int
#include "crypto_int64.h"
#else
#include "crypto_uint64.h"
#endif

#define CHECK1(z,fun,x) \
  if (z != fun(x)) { \
    fprintf(stderr,"%s(%lld) returned %lld, expected %lld\n" \
      ,#fun \
      ,(long long) x \
      ,(long long) fun(x) \
      ,(long long) z \
      ); \
    return 100; \
  }

#define CHECK2(z,fun,x,y)  \
  if (z != fun(x,y)) { \
    fprintf(stderr,"%s(%lld,%lld) returned %lld, expected %lld\n" \
      ,#fun \
      ,(long long) x \
      ,(long long) y \
      ,(long long) fun(x,y) \
      ,(long long) z); \
    return 100; \
  }

int check_x(crypto_uint64 x)
{
  crypto_uint64 z, t;
  int j;

  crypto_uint64_store((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 64;j += 8)
    t += ((crypto_uint64) ((j/8)[(unsigned char *) &z])) << j;
  if (x != t) {
    fprintf(stderr,"crypto_uint64_store failed\n");
    return 100;
  }
  if (x != crypto_uint64_load((unsigned char *) &z)) {
    fprintf(stderr,"crypto_uint64_load failed\n");
    return 100;
  }

  crypto_uint64_store_bigendian((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 64;j += 8)
    t += ((crypto_uint64) ((j/8)[(unsigned char *) &z])) << (64-8-j);
  if (x != t) {
    fprintf(stderr,"crypto_uint64_store_bigendian failed\n");
    return 100;
  }
  if (x != crypto_uint64_load_bigendian((unsigned char *) &z)) {
    fprintf(stderr,"crypto_uint64_load_bigendian failed\n");
    return 100;
  }

#ifdef sign_is_int
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_uint64_negative_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_uint64_negative_01,x)
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_uint64_topbit_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_uint64_topbit_01,x)
#else
  z = (x >> (64-1)) ? -1 : 0; CHECK1(z,crypto_uint64_topbit_mask,x)
  z = (x >> (64-1)) ?  1 : 0; CHECK1(z,crypto_uint64_topbit_01,x)
#endif
  z = (x & 1) ? -1 : 0; CHECK1(z,crypto_uint64_bottombit_mask,x)
  z = (x & 1) ?  1 : 0; CHECK1(z,crypto_uint64_bottombit_01,x)
  for (j = -10*64;j <= 10*64;++j) {
    int jmod64 = ((j % 64) + 64) % 64;
    z = x << jmod64; CHECK2(z,crypto_uint64_shlmod,x,j)
    z = x >> jmod64; CHECK2(z,crypto_uint64_shrmod,x,j)
    z = -((x >> jmod64) & 1); CHECK2(z,crypto_uint64_bitmod_mask,x,j)
    if (j >= 0) if (j < 64) CHECK2(z,crypto_uint64_bitinrangepublicpos_mask,x,j)
    z =  ((x >> jmod64) & 1); CHECK2(z,crypto_uint64_bitmod_01,x,j)
    if (j >= 0) if (j < 64) CHECK2(z,crypto_uint64_bitinrangepublicpos_01,x,j)
  }
  z = x != 0 ? -1 : 0; CHECK1(z,crypto_uint64_nonzero_mask,x)
  z = x != 0 ?  1 : 0; CHECK1(z,crypto_uint64_nonzero_01,x)
  z = x == 0 ? -1 : 0; CHECK1(z,crypto_uint64_zero_mask,x)
  z = x == 0 ?  1 : 0; CHECK1(z,crypto_uint64_zero_01,x)
#ifdef sign_is_int
  z = x > 0 ? -1 : 0; CHECK1(z,crypto_uint64_positive_mask,x)
  z = x > 0 ?  1 : 0; CHECK1(z,crypto_uint64_positive_01,x)
#endif

  z = 0;
  for (j = 0;j < 64;++j) z += 1 & (x >> j);
  CHECK1(z,crypto_uint64_ones_num,x)

  z = 0;
  for (j = 0;j < 64;++j) { if (1 & (x >> j)) break; ++z; }
  CHECK1(z,crypto_uint64_bottomzeros_num,x)

  return 0;
}

int check_xy(crypto_uint64 x,crypto_uint64 y)
{
  crypto_uint64 z, t;

  z = x != y ? -1 : 0; CHECK2(z,crypto_uint64_unequal_mask,x,y)
  z = x != y ?  1 : 0; CHECK2(z,crypto_uint64_unequal_01,x,y)
  z = x == y ? -1 : 0; CHECK2(z,crypto_uint64_equal_mask,x,y)
  z = x == y ?  1 : 0; CHECK2(z,crypto_uint64_equal_01,x,y)
  z = x < y ? -1 : 0; CHECK2(z,crypto_uint64_smaller_mask,x,y)
  z = x < y ?  1 : 0; CHECK2(z,crypto_uint64_smaller_01,x,y)
  z = x <= y ? -1 : 0; CHECK2(z,crypto_uint64_leq_mask,x,y)
  z = x <= y ?  1 : 0; CHECK2(z,crypto_uint64_leq_01,x,y)

  z = x < y ? x : y; CHECK2(z,crypto_uint64_min,x,y)
  t = x > y ? x : y; CHECK2(t,crypto_uint64_max,x,y)
  crypto_uint64_minmax(&x,&y);
  if (z != x) { fprintf(stderr,"crypto_uint64_minmax failed\n"); return 100; }
  if (t != y) { fprintf(stderr,"crypto_uint64_minmax failed\n"); return 100; }

  return 0;
}

int main(int argc,char **argv)
{
  crypto_uint64 one = atoi(argv[1]);
  crypto_uint64 zero = atoi(argv[2]);
  crypto_uint64 x, y, z;
  int i, j, k, l;

  x = one;
  for (i = 0;i < 64;++i) {
    if (x == 0) return 100;
    x += x ^ zero;
  }
  if (x != 0) return 100;
#ifdef sign_is_int
  x -= 1;
  if (x > 0) return 100;
#endif

#ifdef sign_is_int
  for (x = -100;x <= 100;++x) {
    if (check_x(x*one) != 0) return 100;
    for (y = -100;y <= 100;++y) {
      if (check_xy(x*one,y^zero) != 0) return 100;
    }
  }
#else
  for (x = 0;x <= 200;++x) {
    if (check_x(x*one) != 0) return 100;
    for (y = 0;y <= 200;++y) {
      if (check_xy(x*one,y^zero) != 0) return 100;
    }
  }
#endif

  x = 1;
  for (i = 0;i < 64;++i) {
    for (k = -3;k <= 3;++k) {
      if (check_x((k+x)*one) != 0) return 100;
      if (check_x((k-x)*one) != 0) return 100;
      y = 1;
      for (j = 0;j < 64;++j) {
        for (l = -3;l <= 3;++l) {
          if (check_xy((k+x)*one,(l+y)^zero) != 0) return 100;
          if (check_xy((k+x)*one,(l-y)^zero) != 0) return 100;
          if (check_xy((k-x)*one,(l+y)^zero) != 0) return 100;
          if (check_xy((k-x)*one,(l-y)^zero) != 0) return 100;
        }
        y *= 2;
      }
    }
    x *= 2;
  }

  z = one;
  for (i = 0;i < 10000;++i) {
    x ^= zero;
    y *= one;
    x += y;
    y ^= z;
    z += x >> 5;
    y ^= x << 3;
    if (check_x(x) != 0) return 100;
    if (check_xy(x,y) != 0) return 100;
  }

  return 0;
}
