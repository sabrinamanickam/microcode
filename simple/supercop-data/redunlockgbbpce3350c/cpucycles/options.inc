#define NUMOPTIONS 7
#define DEFAULTOPTION (NUMOPTIONS-1)

extern long long supercop_cpucycles_ticks_amd64_tsc_setup(void);
extern long long supercop_cpucycles_ticks_amd64_tsc(void);

extern void supercop_cpucycles_ticks_amd64_tsc_close(void);

extern long long supercop_cpucycles_ticks_amd64_tscasm_setup(void);
extern long long supercop_cpucycles_ticks_amd64_tscasm(void);

extern void supercop_cpucycles_ticks_amd64_tscasm_close(void);

extern long long supercop_cpucycles_ticks_default_perfevent_setup(void);
extern long long supercop_cpucycles_ticks_default_perfevent(void);

extern void supercop_cpucycles_ticks_default_perfevent_close(void);

extern long long supercop_cpucycles_ticks_default_mach_setup(void);
extern long long supercop_cpucycles_ticks_default_mach(void);

extern void supercop_cpucycles_ticks_default_mach_close(void);

extern long long supercop_cpucycles_ticks_default_monotonic_setup(void);
extern long long supercop_cpucycles_ticks_default_monotonic(void);

extern void supercop_cpucycles_ticks_default_monotonic_close(void);

extern long long supercop_cpucycles_ticks_default_gettimeofday_setup(void);
extern long long supercop_cpucycles_ticks_default_gettimeofday(void);

extern void supercop_cpucycles_ticks_default_gettimeofday_close(void);

extern long long supercop_cpucycles_ticks_default_zero_setup(void);
extern long long supercop_cpucycles_ticks_default_zero(void);

extern void supercop_cpucycles_ticks_default_zero_close(void);


static struct {
  const char *implementation;
  long long (*ticks_setup)(void);
  long long (*ticks)(void);
  void (*ticks_close)(void);
} options[NUMOPTIONS] = {
{ "amd64-tsc", supercop_cpucycles_ticks_amd64_tsc_setup, supercop_cpucycles_ticks_amd64_tsc, supercop_cpucycles_ticks_amd64_tsc_close },
{ "amd64-tscasm", supercop_cpucycles_ticks_amd64_tscasm_setup, supercop_cpucycles_ticks_amd64_tscasm, supercop_cpucycles_ticks_amd64_tscasm_close },
{ "default-perfevent", supercop_cpucycles_ticks_default_perfevent_setup, supercop_cpucycles_ticks_default_perfevent, supercop_cpucycles_ticks_default_perfevent_close },
{ "default-mach", supercop_cpucycles_ticks_default_mach_setup, supercop_cpucycles_ticks_default_mach, supercop_cpucycles_ticks_default_mach_close },
{ "default-monotonic", supercop_cpucycles_ticks_default_monotonic_setup, supercop_cpucycles_ticks_default_monotonic, supercop_cpucycles_ticks_default_monotonic_close },
{ "default-gettimeofday", supercop_cpucycles_ticks_default_gettimeofday_setup, supercop_cpucycles_ticks_default_gettimeofday, supercop_cpucycles_ticks_default_gettimeofday_close },
{ "default-zero", supercop_cpucycles_ticks_default_zero_setup, supercop_cpucycles_ticks_default_zero, supercop_cpucycles_ticks_default_zero_close },
} ;
