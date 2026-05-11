#include <stdio.h>
#include <stdlib.h>
#ifdef sign_is_int
#include "crypto_int8.h"
#else
#include "crypto_uint8.h"
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

int check_x(crypto_int8 x)
{
  crypto_int8 z, t;
  int j;

  crypto_int8_store((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 8;j += 8)
    t += ((crypto_int8) ((j/8)[(unsigned char *) &z])) << j;
  if (x != t) {
    fprintf(stderr,"crypto_int8_store failed\n");
    return 100;
  }
  if (x != crypto_int8_load((unsigned char *) &z)) {
    fprintf(stderr,"crypto_int8_load failed\n");
    return 100;
  }

  crypto_int8_store_bigendian((unsigned char *) &z,x);
  t = 0;
  for (j = 0;j < 8;j += 8)
    t += ((crypto_int8) ((j/8)[(unsigned char *) &z])) << (8-8-j);
  if (x != t) {
    fprintf(stderr,"crypto_int8_store_bigendian failed\n");
    return 100;
  }
  if (x != crypto_int8_load_bigendian((unsigned char *) &z)) {
    fprintf(stderr,"crypto_int8_load_bigendian failed\n");
    return 100;
  }

#ifdef sign_is_int
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_int8_negative_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_int8_negative_01,x)
  z = x < 0 ? -1 : 0; CHECK1(z,crypto_int8_topbit_mask,x)
  z = x < 0 ?  1 : 0; CHECK1(z,crypto_int8_topbit_01,x)
#else
  z = (x >> (8-1)) ? -1 : 0; CHECK1(z,crypto_int8_topbit_mask,x)
  z = (x >> (8-1)) ?  1 : 0; CHECK1(z,crypto_int8_topbit_01,x)
#endif
  z = (x & 1) ? -1 : 0; CHECK1(z,crypto_int8_bottombit_mask,x)
  z = (x & 1) ?  1 : 0; CHECK1(z,crypto_int8_bottombit_01,x)
  for (j = -10*8;j <= 10*8;++j) {
    int jmod8 = ((j % 8) + 8) % 8;
    z = x << jmod8; CHECK2(z,crypto_int8_shlmod,x,j)
    z = x >> jmod8; CHECK2(z,crypto_int8_shrmod,x,j)
    z = -((x >> jmod8) & 1); CHECK2(z,crypto_int8_bitmod_mask,x,j)
    if (j >= 0) if (j < 8) CHECK2(z,crypto_int8_bitinrangepublicpos_mask,x,j)
    z =  ((x >> jmod8) & 1); CHECK2(z,crypto_int8_bitmod_01,x,j)
    if (j >= 0) if (j < 8) CHECK2(z,crypto_int8_bitinrangepublicpos_01,x,j)
  }
  z = x != 0 ? -1 : 0; CHECK1(z,crypto_int8_nonzero_mask,x)
  z = x != 0 ?  1 : 0; CHECK1(z,crypto_int8_nonzero_01,x)
  z = x == 0 ? -1 : 0; CHECK1(z,crypto_int8_zero_mask,x)
  z = x == 0 ?  1 : 0; CHECK1(z,crypto_int8_zero_01,x)
#ifdef sign_is_int
  z = x > 0 ? -1 : 0; CHECK1(z,crypto_int8_positive_mask,x)
  z = x > 0 ?  1 : 0; CHECK1(z,crypto_int8_positive_01,x)
#endif

  z = 0;
  for (j = 0;j < 8;++j) z += 1 & (x >> j);
  CHECK1(z,crypto_int8_ones_num,x)

  z = 0;
  for (j = 0;j < 8;++j) { if (1 & (x >> j)) break; ++z; }
  CHECK1(z,crypto_int8_bottomzeros_num,x)

  return 0;
}

int check_xy(crypto_int8 x,crypto_int8 y)
{
  crypto_int8 z, t;

  z = x != y ? -1 : 0; CHECK2(z,crypto_int8_unequal_mask,x,y)
  z = x != y ?  1 : 0; CHECK2(z,crypto_int8_unequal_01,x,y)
  z = x == y ? -1 : 0; CHECK2(z,crypto_int8_equal_mask,x,y)
  z = x == y ?  1 : 0; CHECK2(z,crypto_int8_equal_01,x,y)
  z = x < y ? -1 : 0; CHECK2(z,crypto_int8_smaller_mask,x,y)
  z = x < y ?  1 : 0; CHECK2(z,crypto_int8_smaller_01,x,y)
  z = x <= y ? -1 : 0; CHECK2(z,crypto_int8_leq_mask,x,y)
  z = x <= y ?  1 : 0; CHECK2(z,crypto_int8_leq_01,x,y)

  z = x < y ? x : y; CHECK2(z,crypto_int8_min,x,y)
  t = x > y ? x : y; CHECK2(t,crypto_int8_max,x,y)
  crypto_int8_minmax(&x,&y);
  if (z != x) { fprintf(stderr,"crypto_int8_minmax failed\n"); return 100; }
  if (t != y) { fprintf(stderr,"crypto_int8_minmax failed\n"); return 100; }

  return 0;
}

int main(int argc,char **argv)
{
  crypto_int8 one = atoi(argv[1]);
  crypto_int8 zero = atoi(argv[2]);
  crypto_int8 x, y, z;
  int i, j, k, l;

  x = one;
  for (i = 0;i < 8;++i) {
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
  for (i = 0;i < 8;++i) {
    for (k = -3;k <= 3;++k) {
      if (check_x((k+x)*one) != 0) return 100;
      if (check_x((k-x)*one) != 0) return 100;
      y = 1;
      for (j = 0;j < 8;++j) {
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
