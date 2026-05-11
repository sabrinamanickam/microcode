int not3(int n)
{
  return n != 3;
}

int bytes(int n)
{
  return (n + 7) / 8;
}

long long shr32(long long n)
{
  return n >> 32;
}

double double5(void)
{
  return 5.0;
}

int intbytes(void)
{
  return sizeof(int);
}

int longbytes(void)
{
  return sizeof(long);
}

int rand1(int *in)
{
  static int out[8];
  int t[12]; unsigned int x;
  int r; int i; int loop;

  for (i = 0;i < 12;++i) t[i] = in[i];
  for (i = 0;i < 8;++i) out[i] = in[i];
  x = t[11];
  for (loop = 0;loop < 50;++loop) {
    for (r = 0;r < 16;++r)
      for (i = 0;i < 12;++i) {
        x ^= t[i];
        x = (x<<3)|(x>>29);
        x += in[i];
        x = (x<<2)|(x>>30);
        t[i] += x;
        x += i;
      }
    for (i = 0;i < 8;++i) out[i] ^= t[i + 4];
  }
  return out[0];
}
extern int not3(int);
extern int bytes(int);
extern long long shr32(long long);
extern double double5(void);
extern int longbytes(void);
extern int intbytes(void);
extern int rand1(int *);

int x[12] = {3,1,4,1,5,9,2,6,5,3,5,8};

int main(int argc,char **argv)
{
  if (intbytes() != sizeof(int)) return 100;
  if (longbytes() != sizeof(long)) return 100;

  if (not3(3)) return 100;

  /* on ppc32, gcc -mpowerpc64 produces SIGILL for >>32 */
  if (!not3(shr32(1))) return 100;

  /* on pentium 1, gcc -march=pentium2 produces SIGILL for (...+7)/8 */
  if (bytes(not3(1)) != 1) return 100;

  /* on pentium 1, gcc -march=prescott produces SIGILL for double comparison */
  if (double5() < 0) return 100;

  /* gcc 4.6.1 -m64 -march=core2 -msse4 -O3 sometimes generates pinsrd */
  if (rand1(x) != -131401890) return 100;

  return 0;
}
