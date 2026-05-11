#include <stdio.h>
#include <stdlib.h>
#ifdef sign_is_int
#include "crypto_int16.h"
#else
#include "crypto_uint16.h"
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

int check_x(crypto_int16 x)
{
  crypto_int16 z, t;
  int j;

  crypto_int16_store((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 16;j += 8)
    t += ((crypto_int16) ((j/8)[(unsigned char *) &z])) << j;
  if (x != t) {
    fprintf(stderr,"crypto_int16_store failed\n");
    return 100;
  }
  if (x != crypto_int16_load((unsigned char *) &z)) {
    fprintf(stderr,"crypto_int16_load failed\n");
    return 100;
  }

  crypto_int16_store_bigendian((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 16;j += 8)
    t += ((crypto_int16) ((j/8)[(unsigned char *) &z])) << (16-8-j);
  if (x != t) {
    fprintf(stderr,"crypto_int16_store_bigendian failed\n");
    return 100;
  }
  if (x != crypto_int16_load_bigendian((unsigned char *) &z)) {
    fprintf(stderr,"crypto_int16_load_bigendian failed\n");
    return 100;
  }

#ifdef sign_is_int
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_int16_negative_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_int16_negative_01,x)
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_int16_topbit_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_int16_topbit_01,x)
#else
  z = (x >> (16-1)) ? -1 : 0; CHECK1(z,crypto_int16_topbit_mask,x)
  z = (x >> (16-1)) ?  1 : 0; CHECK1(z,crypto_int16_topbit_01,x)
#endif
  z = (x & 1) ? -1 : 0; CHECK1(z,crypto_int16_bottombit_mask,x)
  z = (x & 1) ?  1 : 0; CHECK1(z,crypto_int16_bottombit_01,x)
  for (j = -10*16;j <= 10*16;++j) {
    int jmod16 = ((j % 16) + 16) % 16;
    z = x << jmod16; CHECK2(z,crypto_int16_shlmod,x,j)
    z = x >> jmod16; CHECK2(z,crypto_int16_shrmod,x,j)
    z = -((x >> jmod16) & 1); CHECK2(z,crypto_int16_bitmod_mask,x,j)
    if (j >= 0) if (j < 16) CHECK2(z,crypto_int16_bitinrangepublicpos_mask,x,j)
    z =  ((x >> jmod16) & 1); CHECK2(z,crypto_int16_bitmod_01,x,j)
    if (j >= 0) if (j < 16) CHECK2(z,crypto_int16_bitinrangepublicpos_01,x,j)
  }
  z = x != 0 ? -1 : 0; CHECK1(z,crypto_int16_nonzero_mask,x)
  z = x != 0 ?  1 : 0; CHECK1(z,crypto_int16_nonzero_01,x)
  z = x == 0 ? -1 : 0; CHECK1(z,crypto_int16_zero_mask,x)
  z = x == 0 ?  1 : 0; CHECK1(z,crypto_int16_zero_01,x)
#ifdef sign_is_int
  z = x > 0 ? -1 : 0; CHECK1(z,crypto_int16_positive_mask,x)
  z = x > 0 ?  1 : 0; CHECK1(z,crypto_int16_positive_01,x)
#endif

  z = 0;
  for (j = 0;j < 16;++j) z += 1 & (x >> j);
  CHECK1(z,crypto_int16_ones_num,x)

  z = 0;
  for (j = 0;j < 16;++j) { if (1 & (x >> j)) break; ++z; }
  CHECK1(z,crypto_int16_bottomzeros_num,x)

  return 0;
}

int check_xy(crypto_int16 x,crypto_int16 y)
{
  crypto_int16 z, t;

  z = x != y ? -1 : 0; CHECK2(z,crypto_int16_unequal_mask,x,y)
  z = x != y ?  1 : 0; CHECK2(z,crypto_int16_unequal_01,x,y)
  z = x == y ? -1 : 0; CHECK2(z,crypto_int16_equal_mask,x,y)
  z = x == y ?  1 : 0; CHECK2(z,crypto_int16_equal_01,x,y)
  z = x < y ? -1 : 0; CHECK2(z,crypto_int16_smaller_mask,x,y)
  z = x < y ?  1 : 0; CHECK2(z,crypto_int16_smaller_01,x,y)
  z = x <= y ? -1 : 0; CHECK2(z,crypto_int16_leq_mask,x,y)
  z = x <= y ?  1 : 0; CHECK2(z,crypto_int16_leq_01,x,y)

  z = x < y ? x : y; CHECK2(z,crypto_int16_min,x,y)
  t = x > y ? x : y; CHECK2(t,crypto_int16_max,x,y)
  crypto_int16_minmax(&x,&y);
  if (z != x) { fprintf(stderr,"crypto_int16_minmax failed\n"); return 100; }
  if (t != y) { fprintf(stderr,"crypto_int16_minmax failed\n"); return 100; }

  return 0;
}

int main(int argc,char **argv)
{
  crypto_int16 one = atoi(argv[1]);
  crypto_int16 zero = atoi(argv[2]);
  crypto_int16 x, y, z;
  int i, j, k, l;

  x = one;
  for (i = 0;i < 16;++i) {
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
  for (i = 0;i < 16;++i) {
    for (k = -3;k <= 3;++k) {
      if (check_x((k+x)*one) != 0) return 100;
      if (check_x((k-x)*one) != 0) return 100;
      y = 1;
      for (j = 0;j < 16;++j) {
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
