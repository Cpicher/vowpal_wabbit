/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#ifdef _WIN32
#pragma warning (disable : 4996)  // generated by inner_product use
#endif
#include <fstream>
#include <vector>
#include <queue>
#include <algorithm>
#include <numeric>
#include <cmath>
#include "correctedMath.h"
#include "vw_versions.h"
#include "vw.h"
#include "mwt.h"

#include <boost/math/special_functions/digamma.hpp>
#include <boost/math/special_functions/gamma.hpp>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netdb.h>
#endif

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "gd.h"
#include "rand48.h"
#include "reductions.h"
#include "array_parameters.h"
#include <boost/version.hpp>

#if BOOST_VERSION >= 105600
#include <boost/align/is_aligned.hpp>
#endif
using namespace std;

enum lda_math_mode { USE_SIMD, USE_PRECISE, USE_FAST_APPROX };

class index_feature
{
public:
  uint32_t document;
  feature f;
  bool operator<(const index_feature b) const { return f.weight_index < b.f.weight_index; }
};

struct lda
{ size_t topics;
  float lda_alpha;
  float lda_rho;
  float lda_D;
  float lda_epsilon;
  size_t minibatch;
  lda_math_mode mmode;

  v_array<float> Elogtheta;
  v_array<float> decay_levels;
  v_array<float> total_new;
  v_array<example *> examples;
  v_array<float> total_lambda;
  v_array<int> doc_lengths;
  v_array<float> digammas;
  v_array<float> v;
  std::vector<index_feature> sorted_features;

  bool compute_coherence_metrics;

  // size by 1 << bits
  std::vector<uint32_t> feature_counts;
  std::vector<std::vector<size_t>> feature_to_example_map;

  bool total_lambda_init;

  double example_t;
  vw *all; // regressor, lda

// constexpr is not supported in VS2013 (except in one CTP release)
// if it makes it into VS 2015 change the next ifdef to check Visual Studio Release

  // static constexpr float underflow_threshold = 1.0e-10f;
  inline const float  underflow_threshold() { return 1.0e-10f; }

  inline float digamma(float x);
  inline float lgamma(float x);
  inline float powf(float x, float p);
  inline void expdigammify(vw &all, float *gamma);
  inline void expdigammify_2(vw &all, weight_parameters::iterator gamma, float *norm);
};

// #define VW_NO_INLINE_SIMD

namespace
{
inline bool is_aligned16(void *ptr)
{
#if BOOST_VERSION >= 105600
  return boost::alignment::is_aligned(16, ptr);
#else
  return ((reinterpret_cast<uintptr_t>(ptr) & 0x0f) == 0);
#endif
}
}

namespace ldamath
{
inline float fastlog2(float x)
{ uint32_t mx;
  memcpy(&mx, &x, sizeof(uint32_t));
  mx = (mx & 0x007FFFFF) | (0x7e << 23);

  float mx_f;
  memcpy(&mx_f, &mx, sizeof(float));

  uint32_t vx;
  memcpy(&vx, &x, sizeof(uint32_t));

  float y = static_cast<float>(vx);
  y *= 1.0f / (float)(1 << 23);

  return y - 124.22544637f - 1.498030302f * mx_f - 1.72587999f / (0.3520887068f + mx_f);
}

inline float fastlog(float x) { return 0.69314718f * fastlog2(x); }

inline float fastpow2(float p)
{ float offset = (p < 0) * 1.0f;
  float clipp = (p < -126.0) ? -126.0f : p;
  int w = (int)clipp;
  float z = clipp - w + offset;
  uint32_t approx = (uint32_t) ((1 << 23) * (clipp + 121.2740838f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z));

  float v;
  memcpy(&v, &approx, sizeof(uint32_t));
  return v;
}

inline float fastexp(float p) { return fastpow2(1.442695040f * p); }

inline float fastpow(float x, float p) { return fastpow2(p * fastlog2(x)); }

inline float fastlgamma(float x)
{ float logterm = fastlog(x * (1.0f + x) * (2.0f + x));
  float xp3 = 3.0f + x;

  return -2.081061466f - x + 0.0833333f / xp3 - logterm + (2.5f + x) * fastlog(xp3);
}

inline float fastdigamma(float x)
{ float twopx = 2.0f + x;
  float logterm = fastlog(twopx);

  return -(1.0f + 2.0f * x) / (x * (1.0f + x)) - (13.0f + 6.0f * x) / (12.0f * twopx * twopx) + logterm;
}

#if !defined(VW_NO_INLINE_SIMD)

#if defined(__SSE2__) || defined(__SSE3__) || defined(__SSE4_1__)

// Include headers for the various SSE versions:
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE3__)
#include <tmmintrin.h>
#endif
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

#define HAVE_SIMD_MATHMODE

typedef __m128 v4sf;
typedef __m128i v4si;

inline v4sf v4si_to_v4sf(v4si x) { return _mm_cvtepi32_ps(x); }

inline v4si v4sf_to_v4si(v4sf x) { return _mm_cvttps_epi32(x); }

// Extract v[idx]
template <const int idx> float v4sf_index(const v4sf x)
{
#if defined(__SSE4_1__)
  float ret;
  uint32_t val;

  val = _mm_extract_ps(x, idx);
  // Portably convert uint32_t bit pattern to float. Optimizers will generally
  // make this disappear.
  memcpy(&ret, &val, sizeof(uint32_t));
  return ret;
#else
  return _mm_cvtss_f32(_mm_shuffle_ps(x, x, _MM_SHUFFLE(idx, idx, idx, idx)));
#endif
}

// Specialization for the 0'th element
template <> float v4sf_index<0>(const v4sf x) { return _mm_cvtss_f32(x); }

inline const v4sf v4sfl(const float x) { return _mm_set1_ps(x); }

inline const v4si v4sil(const uint32_t x) { return _mm_set1_epi32(x); }

#ifdef WIN32

inline __m128 operator+(const __m128 a, const __m128 b)
{
	return _mm_add_ps(a, b);
}

inline __m128 operator-(const __m128 a, const __m128 b)
{
	return _mm_sub_ps(a, b);
}

inline __m128 operator*(const __m128 a, const __m128 b)
{
	return _mm_mul_ps(a, b);
}

inline __m128 operator/(const __m128 a, const __m128 b)
{
	return _mm_div_ps(a, b);
}

#endif

inline v4sf vfastpow2(const v4sf p)
{ v4sf ltzero = _mm_cmplt_ps(p, v4sfl(0.0f));
  v4sf offset = _mm_and_ps(ltzero, v4sfl(1.0f));
  v4sf lt126 = _mm_cmplt_ps(p, v4sfl(-126.0f));
  v4sf clipp = _mm_andnot_ps(lt126, p) + _mm_and_ps(lt126, v4sfl(-126.0f));
  v4si w = v4sf_to_v4si(clipp);
  v4sf z = clipp - v4si_to_v4sf(w) + offset;

  const v4sf c_121_2740838 = v4sfl(121.2740838f);
  const v4sf c_27_7280233 = v4sfl(27.7280233f);
  const v4sf c_4_84252568 = v4sfl(4.84252568f);
  const v4sf c_1_49012907 = v4sfl(1.49012907f);

  v4sf v = v4sfl(1 << 23) * (clipp + c_121_2740838 + c_27_7280233 / (c_4_84252568 - z) - c_1_49012907 * z);

  return _mm_castsi128_ps(v4sf_to_v4si(v));
}

inline v4sf vfastexp(const v4sf p)
{ const v4sf c_invlog_2 = v4sfl(1.442695040f);

  return vfastpow2(c_invlog_2 * p);
}

inline v4sf vfastlog2(v4sf x)
{ v4si vx_i = _mm_castps_si128(x);
  v4sf mx_f = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(vx_i, v4sil(0x007FFFFF)), v4sil(0x3f000000)));
  v4sf y = v4si_to_v4sf(vx_i) * v4sfl(1.1920928955078125e-7f);

  const v4sf c_124_22551499 = v4sfl(124.22551499f);
  const v4sf c_1_498030302 = v4sfl(1.498030302f);
  const v4sf c_1_725877999 = v4sfl(1.72587999f);
  const v4sf c_0_3520087068 = v4sfl(0.3520887068f);

  return y - c_124_22551499 - c_1_498030302 * mx_f - c_1_725877999 / (c_0_3520087068 + mx_f);
}

inline v4sf vfastlog(v4sf x)
{ const v4sf c_0_69314718 = v4sfl(0.69314718f);

  return c_0_69314718 * vfastlog2(x);
}

inline v4sf vfastdigamma(v4sf x)
{ v4sf twopx = v4sfl(2.0f) + x;
  v4sf logterm = vfastlog(twopx);

  return (v4sfl(-48.0f) + x * (v4sfl(-157.0f) + x * (v4sfl(-127.0f) - v4sfl(30.0f) * x))) /
         (v4sfl(12.0f) * x * (v4sfl(1.0f) + x) * twopx * twopx) +
         logterm;
}

void vexpdigammify(vw &all, float *gamma, const float underflow_threshold)
{ float extra_sum = 0.0f;
  v4sf sum = v4sfl(0.0f);
  float *fp;
  const float *fpend = gamma + all.lda;

  // Iterate through the initial part of the array that isn't 128-bit SIMD
  // aligned.
  for (fp = gamma; fp < fpend && !is_aligned16(fp); ++fp)
  { extra_sum += *fp;
    *fp = fastdigamma(*fp);
  }

  // Rip through the aligned portion...
  for (; is_aligned16(fp) && fp + 4 < fpend; fp += 4)
  { v4sf arg = _mm_load_ps(fp);
    sum = sum + arg;
    arg = vfastdigamma(arg);
    _mm_store_ps(fp, arg);
  }

  for (; fp < fpend; ++fp)
  { extra_sum += *fp;
    *fp = fastdigamma(*fp);
  }

#if defined(__SSE3__) || defined(__SSE4_1__)
  // Do two horizontal adds on sum, extract the total from the 0 element:
  sum = _mm_hadd_ps(sum, sum);
  sum = _mm_hadd_ps(sum, sum);
  extra_sum += v4sf_index<0>(sum);
#else
  extra_sum += v4sf_index<0>(sum) + v4sf_index<1>(sum) + v4sf_index<2>(sum) + v4sf_index<3>(sum);
#endif

  extra_sum = fastdigamma(extra_sum);
  sum = v4sfl(extra_sum);

  for (fp = gamma; fp < fpend && !is_aligned16(fp); ++fp)
  { *fp = fmax(underflow_threshold, fastexp(*fp - extra_sum));
  }

  for (; is_aligned16(fp) && fp + 4 < fpend; fp += 4)
  { v4sf arg = _mm_load_ps(fp);
    arg = arg - sum;
    arg = vfastexp(arg);
    arg = _mm_max_ps(v4sfl(underflow_threshold), arg);
    _mm_store_ps(fp, arg);
  }

  for (; fp < fpend; ++fp)
  { *fp = fmax(underflow_threshold, fastexp(*fp - extra_sum));
  }
}

void vexpdigammify_2(vw &all, weight_parameters::iterator gamma, const float *norm, const float underflow_threshold)
{ weight_parameters::iterator fp = gamma;
  const float *np;
  uint32_t count = 0;

  for (np = norm; count < all.lda && !is_aligned16(&(*fp)); ++fp, ++np, ++count)
  { *fp = fmax(underflow_threshold, fastexp(fastdigamma(*fp) - *np));
  }

  for (; is_aligned16(&(*fp)) && count + 4 < all.lda; fp += 4, np += 4, count += 4)
  { v4sf arg = _mm_load_ps(&(*fp));
    arg = vfastdigamma(arg);
    v4sf vnorm = _mm_loadu_ps(np);
    arg = arg - vnorm;
    arg = vfastexp(arg);
    arg = _mm_max_ps(v4sfl(underflow_threshold), arg);
    _mm_store_ps(&(*fp), arg);
  }

  for (; count < all.lda ; ++fp, ++np, ++count)
  { *fp = fmax(underflow_threshold, fastexp(fastdigamma(*fp) - *np));
  }
}

#else
// PLACEHOLDER for future ARM NEON code
// Also remember to define HAVE_SIMD_MATHMODE
#endif

#endif // !VW_NO_INLINE_SIMD

// Templates for common code shared between the three math modes (SIMD, fast approximations
// and accurate).
//
// The generic template takes a type and a specialization flag, mtype.
//
// mtype == USE_PRECISE: Use the accurate computation for lgamma, digamma.
// mtype == USE_FAST_APPROX: Use the fast approximations for lgamma, digamma.
// mtype == USE_SIMD: Use CPU SIMD instruction
//
// The generic template is specialized for the particular accuracy setting.

// Log gamma:
template <typename T, const lda_math_mode mtype> inline T lgamma(T x)
{ BOOST_STATIC_ASSERT_MSG(true, "ldamath::lgamma is not defined for this type and math mode.");
}

// Digamma:
template <typename T, const lda_math_mode mtype> inline T digamma(T x)
{ BOOST_STATIC_ASSERT_MSG(true, "ldamath::digamma is not defined for this type and math mode.");
}

// Exponential
template <typename T, lda_math_mode mtype> inline T exponential(T x)
{ BOOST_STATIC_ASSERT_MSG(true, "ldamath::exponential is not defined for this type and math mode.");
}

// Powf
template <typename T, lda_math_mode mtype> inline T powf(T x, T p)
{ BOOST_STATIC_ASSERT_MSG(true, "ldamath::powf is not defined for this type and math mode.");
}

// High accuracy float specializations:

template <> inline float lgamma<float, USE_PRECISE>(float x) { return boost::math::lgamma(x); }
template <> inline float digamma<float, USE_PRECISE>(float x) { return boost::math::digamma(x); }
template <> inline float exponential<float, USE_PRECISE>(float x) { return correctedExp(x); }
template <> inline float powf<float, USE_PRECISE>(float x, float p) { return std::pow(x, p); }

// Fast approximation float specializations:

template <> inline float lgamma<float, USE_FAST_APPROX>(float x) { return fastlgamma(x); }
template <> inline float digamma<float, USE_FAST_APPROX>(float x) { return fastdigamma(x); }
template <> inline float exponential<float, USE_FAST_APPROX>(float x) { return fastexp(x); }
template <> inline float powf<float, USE_FAST_APPROX>(float x, float p) { return fastpow(x, p); }

// SIMD specializations:

template <> inline float lgamma<float, USE_SIMD>(float x) { return lgamma<float, USE_FAST_APPROX>(x); }
template <> inline float digamma<float, USE_SIMD>(float x) { return digamma<float, USE_FAST_APPROX>(x); }
template <> inline float exponential<float, USE_SIMD>(float x) { return exponential<float, USE_FAST_APPROX>(x); }
template <> inline float powf<float, USE_SIMD>(float x, float p) { return powf<float, USE_FAST_APPROX>(x, p); }

template <typename T, const lda_math_mode mtype> inline void expdigammify(vw &all, T *gamma, T threshold, T initial)
{ T sum = digamma<T, mtype>(std::accumulate(gamma, gamma + all.lda, initial));

  std::transform(gamma, gamma + all.lda, gamma, [sum, threshold](T g)
  { return fmax(threshold, exponential<T, mtype>(digamma<T, mtype>(g) - sum));
  });
}
template <> inline void expdigammify<float, USE_SIMD>(vw &all, float *gamma, float threshold, float)
{
#if defined(HAVE_SIMD_MATHMODE)
  vexpdigammify(all, gamma, threshold);
#else
  // Do something sensible if SIMD math isn't available:
  expdigammify<float, USE_FAST_APPROX>(all, gamma, threshold, 0.0);
#endif
}

template <typename T, const lda_math_mode mtype>
inline void expdigammify_2(vw &all, weight_parameters::iterator gamma, T *norm, const T threshold)
{ std::transform(gamma, gamma + all.lda, norm, gamma, [threshold](float g, float n)
  { return fmax(threshold, exponential<T, mtype>(digamma<T, mtype>(g) - n));
  });
}
template <> inline void expdigammify_2<float, USE_SIMD>(vw &all, weight_parameters::iterator gamma, float *norm, const float threshold)
{
#if defined(HAVE_SIMD_MATHMODE)
  vexpdigammify_2(all, gamma, norm, threshold);
#else
  // Do something sensible if SIMD math isn't available:
  expdigammify_2<float, USE_FAST_APPROX>(all, gamma, norm, threshold);
#endif
}
} // namespace ldamath

float lda::digamma(float x)
{ switch (mmode)
  { case USE_FAST_APPROX:
      // std::cerr << "lda::digamma FAST_APPROX ";
      return ldamath::digamma<float, USE_FAST_APPROX>(x);
    case USE_PRECISE:
      // std::cerr << "lda::digamma PRECISE ";
      return ldamath::digamma<float, USE_PRECISE>(x);
    case USE_SIMD:
      // std::cerr << "lda::digamma SIMD ";
      return ldamath::digamma<float, USE_SIMD>(x);
    default:
      // Should not happen.
      std::cerr << "lda::digamma: Trampled or invalid math mode, aborting" << std::endl;
      abort();
      return 0.0f;
  }
}

float lda::lgamma(float x)
{ switch (mmode)
  { case USE_FAST_APPROX:
      // std::cerr << "lda::lgamma FAST_APPROX ";
      return ldamath::lgamma<float, USE_FAST_APPROX>(x);
    case USE_PRECISE:
      // std::cerr << "lda::lgamma PRECISE ";
      return ldamath::lgamma<float, USE_PRECISE>(x);
    case USE_SIMD:
      // std::cerr << "lda::gamma SIMD ";
      return ldamath::lgamma<float, USE_SIMD>(x);
    default:
      std::cerr << "lda::lgamma: Trampled or invalid math mode, aborting" << std::endl;
      abort();
      return 0.0f;
  }
}

float lda::powf(float x, float p)
{ switch (mmode)
  { case USE_FAST_APPROX:
      // std::cerr << "lda::powf FAST_APPROX ";
      return ldamath::powf<float, USE_FAST_APPROX>(x, p);
    case USE_PRECISE:
      // std::cerr << "lda::powf PRECISE ";
      return ldamath::powf<float, USE_PRECISE>(x, p);
    case USE_SIMD:
      // std::cerr << "lda::powf SIMD ";
      return ldamath::powf<float, USE_SIMD>(x, p);
    default:
      std::cerr << "lda::powf: Trampled or invalid math mode, aborting" << std::endl;
      abort();
      return 0.0f;
  }
}

void lda::expdigammify(vw &all, float *gamma)
{ switch (mmode)
  { case USE_FAST_APPROX:
      ldamath::expdigammify<float, USE_FAST_APPROX>(all, gamma, underflow_threshold(), 0.0f);
      break;
    case USE_PRECISE:
      ldamath::expdigammify<float, USE_PRECISE>(all, gamma, underflow_threshold(), 0.0f);
      break;
    case USE_SIMD:
      ldamath::expdigammify<float, USE_SIMD>(all, gamma, underflow_threshold(), 0.0f);
      break;
    default:
      std::cerr << "lda::expdigammify: Trampled or invalid math mode, aborting" << std::endl;
      abort();
  }
}

void lda::expdigammify_2(vw &all, weight_parameters::iterator gamma, float *norm)
{ switch (mmode)
  { case USE_FAST_APPROX:
      ldamath::expdigammify_2<float, USE_FAST_APPROX>(all, gamma, norm, underflow_threshold());
      break;
    case USE_PRECISE:
      ldamath::expdigammify_2<float, USE_PRECISE>(all, gamma, norm, underflow_threshold());
      break;
    case USE_SIMD:
      ldamath::expdigammify_2<float, USE_SIMD>(all, gamma, norm, underflow_threshold());
      break;
    default:
      std::cerr << "lda::expdigammify_2: Trampled or invalid math mode, aborting" << std::endl;
      abort();
  }
}

static inline float average_diff(vw &all, float *oldgamma, float *newgamma)
{ float sum;
  float normalizer;

  // This warps the normal sense of "inner product", but it accomplishes the same
  // thing as the "plain old" for loop. clang does a good job of reducing the
  // common subexpressions.
  sum = std::inner_product(oldgamma, oldgamma + all.lda, newgamma, 0.0f,
  [](float accum, float absdiff) { return accum + absdiff; },
  [](float old_g, float new_g) { return std::abs(old_g - new_g); });

  normalizer = std::accumulate(newgamma, newgamma + all.lda, 0.0f);
  return sum / normalizer;
}

// Returns E_q[log p(\theta)] - E_q[log q(\theta)].
float theta_kl(lda &l, v_array<float> &Elogtheta, float *gamma)
{ float gammasum = 0;
  Elogtheta.erase();
  for (size_t k = 0; k < l.topics; k++)
  { Elogtheta.push_back(l.digamma(gamma[k]));
    gammasum += gamma[k];
  }
  float digammasum = l.digamma(gammasum);
  gammasum = l.lgamma(gammasum);
  float kl = -(l.topics * l.lgamma(l.lda_alpha));
  kl += l.lgamma(l.lda_alpha * l.topics) - gammasum;
  for (size_t k = 0; k < l.topics; k++)
  { Elogtheta[k] -= digammasum;
    kl += (l.lda_alpha - gamma[k]) * Elogtheta[k];
    kl += l.lgamma(gamma[k]);
  }

  return kl;
}

static inline float find_cw(lda &l, weight_parameters::iterator u_for_w, float *v)
{ return 1.0f / std::inner_product(u_for_w, u_for_w + l.topics, v, 0.0f);
}

namespace
{
// Effectively, these are static and not visible outside the compilation unit.
v_array<float> new_gamma = v_init<float>();
v_array<float> old_gamma = v_init<float>();
}

// Returns an estimate of the part of the variational bound that
// doesn't have to do with beta for the entire corpus for the current
// setting of lambda based on the document passed in. The value is
// divided by the total number of words in the document This can be
// used as a (possibly very noisy) estimate of held-out likelihood.
float lda_loop(lda &l, v_array<float> &Elogtheta, float *v, weight_parameters& weights, example *ec, float)
{ new_gamma.erase();
  old_gamma.erase();

  for (size_t i = 0; i < l.topics; i++)
  { new_gamma.push_back(1.f);
    old_gamma.push_back(0.f);
  }
  size_t num_words = 0;
  for (features& fs : *ec)
    num_words += fs.size();

  float xc_w = 0;
  float score = 0;
  float doc_length = 0;
  do
  { memcpy(v, new_gamma.begin(), sizeof(float) * l.topics);
    l.expdigammify(*l.all, v);

    memcpy(old_gamma.begin(), new_gamma.begin(), sizeof(float) * l.topics);
    memset(new_gamma.begin(), 0, sizeof(float) * l.topics);

    score = 0;
    size_t word_count = 0;
    doc_length = 0;
    for (features& fs : *ec)
      { for (features::iterator& f : fs)
	     {  weight_parameters::iterator u_for_w = weights.change_begin() + (f.index() & weights.mask()) + l.topics + 1;
            float c_w = find_cw(l, u_for_w, v);
            xc_w = c_w * f.value();
            score += -f.value() * log(c_w);
            size_t max_k = l.topics;
            for (size_t k = 0; k < max_k; k++, ++u_for_w)
              new_gamma[k] += xc_w * *u_for_w;
            word_count++;
            doc_length += f.value();
          }
      }
    for (size_t k = 0; k < l.topics; k++)
      new_gamma[k] = new_gamma[k] * v[k] + l.lda_alpha;
  }
  while (average_diff(*l.all, old_gamma.begin(), new_gamma.begin()) > l.lda_epsilon);

  ec->pred.scalars.erase();
  ec->pred.scalars.resize(l.topics);
  memcpy(ec->pred.scalars.begin(), new_gamma.begin(), l.topics * sizeof(float));
  ec->pred.scalars.end() = ec->pred.scalars.begin() + l.topics;

  score += theta_kl(l, Elogtheta, new_gamma.begin());

  return score / doc_length;
}

size_t next_pow2(size_t x)
{ int i = 0;
  x = x > 0 ? x - 1 : 0;
  while (x > 0)
  { x >>= 1;
    i++;
  }
  return ((size_t)1) << i;
}

struct initial_weights
{
private:
	weight _initial;
	weight _initial_random;
	bool _random;
	uint32_t _lda;
public:
	initial_weights(weight initial, weight initial_random, bool random, uint32_t lda ) 
		: _initial(initial), _initial_random(initial_random), _random(random), _lda(lda)
	{}
	void operator()(weight_parameters::iterator& iter, uint64_t index)
	{ if (_random)
	  { for (weights_iterator_iterator<weight> k = iter.begin(); k != iter.end(_lda); ++k, ++index)
		{  *k = (float)(-log(merand48(index) + 1e-6) + 1.0f);
		   *k *= _initial_random;
		}
	  }
	(&(*iter))[_lda] = _initial;
	}
};


void save_load(lda &l, io_buf &model_file, bool read, bool text)
{ vw *all = l.all;
  uint64_t length = (uint64_t)1 << all->num_bits;
  if (read)
  { initialize_regressor(*all);
    initial_weights init(all->initial_t, (float)(l.lda_D / all->lda / all->length() * 200), all->random_weights, all->lda);
    all->weights.set_default<initial_weights>(init);

  }
  if (model_file.files.size() > 0)
  { uint64_t i = 0;
    stringstream msg;
    size_t brw = 1;
	weight_parameters& weights = all->weights;
	do
    { brw = 0;

	  if (!read && text)
		  msg << i << " ";

	  if (!read || l.all->model_file_ver >= VERSION_FILE_WITH_HEADER_ID)
		brw += bin_text_read_write_fixed(model_file, (char *)&i, sizeof(i), "", read, msg, text);
	  else
	  {
		  // support 32bit build models
		  uint32_t j;
		  brw += bin_text_read_write_fixed(model_file, (char *)&j, sizeof(j), "", read, msg, text);
		  i = j;
	  }
	  
	  if (brw != 0)
	  { weight_parameters::iterator iter = weights.begin() + i;
		for (weights_iterator_iterator<weight> v = iter.begin(); v != iter.end(all->lda); ++v)
		{ if (!read && text)
		    msg << *v + l.lda_rho << " ";
		  brw += bin_text_read_write_fixed(model_file, (char *)&(*v), sizeof(*v), "", read, msg, text);
		}
	  }
      if (text)
        {
		  if (!read)
			  msg << "\n";
          brw += bin_text_read_write_fixed(model_file, nullptr, 0, "", read, msg, text);
        }
	  if (!read)
		  ++i;
    }
    while ((!read && i < length) || (read && brw > 0));
  }
}

void return_example(vw& all, example& ec)
{
  label_data ld = ec.l.simple;
  
  all.sd->update(ec.test_only, ec.loss, ec.weight, ec.num_features);
  if (ld.label != FLT_MAX && !ec.test_only)
    all.sd->weighted_labels += ld.label * ec.weight;
  all.sd->weighted_unlabeled_examples += ld.label == FLT_MAX ? ec.weight : 0;
  
  for (int f: all.final_prediction_sink)
    MWT::print_scalars(f, ec.pred.scalars, ec.tag);
  
  if (all.sd->weighted_examples >= all.sd->dump_interval && !all.quiet)
    all.sd->print_update(all.holdout_set_off, all.current_pass, ec.l.simple.label, 0.f,
			 ec.num_features, all.progress_add, all.progress_arg);
  VW::finish_example(all,&ec);
}

void learn_batch(lda &l)
{ if (l.sorted_features.empty()) // FAST-PASS for real "true"
  { // This can happen when the socket connection is dropped by the client.
    // If l.sorted_features is empty, then l.sorted_features[0] does not
    // exist, so we should not try to take its address in the beginning of
    // the for loops down there. Since it seems that there's not much to
    // do in this case, we just return.
    for (size_t d = 0; d < l.examples.size(); d++)
      {
	l.examples[d]->pred.scalars.erase();
	l.examples[d]->pred.scalars.resize(l.topics);
	memset(l.examples[d]->pred.scalars.begin(), 0, l.topics * sizeof(float));
	l.examples[d]->pred.scalars.end() = l.examples[d]->pred.scalars.begin() + l.topics;

	l.examples[d]->pred.scalars.erase();
	return_simple_example(*l.all, nullptr, *l.examples[d]);
      }
    l.examples.erase();
    return;
  }

  float eta = -1;
  float minuseta = -1;

  if (l.total_lambda.size() == 0)
  { for (size_t k = 0; k < l.all->lda; k++)
      l.total_lambda.push_back(0.f);

	weight_parameters& weights = l.all->weights;
	size_t stride = 1 << weights.stride_shift();
	weight_parameters::iterator iter = weights.begin();
	for (size_t i = 0; i <= weights.mask(); i += stride, ++iter) 
	{  weights_iterator_iterator<weight> k_iter = iter.begin();
	   for (size_t k = 0; k < l.all->lda; k++, ++k_iter)
			l.total_lambda[k] += *k_iter;
	}   
  }

  l.example_t++;
  l.total_new.erase();
  for (size_t k = 0; k < l.all->lda; k++)
    l.total_new.push_back(0.f);

  size_t batch_size = l.examples.size();

  sort(l.sorted_features.begin(), l.sorted_features.end());

  eta = l.all->eta * l.powf((float)l.example_t, -l.all->power_t);
  minuseta = 1.0f - eta;
  eta *= l.lda_D / batch_size;
  l.decay_levels.push_back(l.decay_levels.last() + log(minuseta));

  l.digammas.erase();
  float additional = (float)(l.all->length()) * l.lda_rho;
  for (size_t i = 0; i < l.all->lda; i++)
  { l.digammas.push_back(l.digamma(l.total_lambda[i] + additional));
  }

  weight_parameters& weights = l.all->weights;

  uint64_t last_weight_index = -1;
  for (index_feature *s = &l.sorted_features[0]; s <= &l.sorted_features.back(); s++)
  { if (last_weight_index == s->f.weight_index)
      continue;
    last_weight_index = s->f.weight_index;
    //float *weights_for_w = &(weights[s->f.weight_index]);
	weight_parameters::iterator weights_for_w = weights.change_begin() + (s->f.weight_index & weights.mask());
    float decay_component =
      l.decay_levels.end()[-2] - l.decay_levels.end()[(int)(-1 - l.example_t + *(weights_for_w + l.all->lda))];
    float decay = fmin(1.0f, correctedExp(decay_component));
     weight_parameters::iterator u_for_w = weights_for_w + l.all->lda + 1;

    *(weights_for_w + l.all->lda) = (float)l.example_t;
    for (size_t k = 0; k < l.all->lda; k++, ++weights_for_w, ++u_for_w)
    { *weights_for_w *= decay;
      *u_for_w = *weights_for_w + l.lda_rho;
    }
	u_for_w = weights.change_begin() + (s->f.weight_index & weights.mask()) +l.all->lda + 1;
    l.expdigammify_2(*l.all, u_for_w, l.digammas.begin());
  }

  for (size_t d = 0; d < batch_size; d++)
  { float score = lda_loop(l, l.Elogtheta, &(l.v[d * l.all->lda]), weights, l.examples[d], l.all->power_t);
    if (l.all->audit)
      GD::print_audit_features(*l.all, *l.examples[d]);
    // If the doc is empty, give it loss of 0.
    if (l.doc_lengths[d] > 0)
    { l.all->sd->sum_loss -= score;
      l.all->sd->sum_loss_since_last_dump -= score;
    }
    return_simple_example(*l.all, nullptr, *l.examples[d]);
  }

  // -t there's no need to update weights (especially since it's a noop)
  if (eta != 0)
  {
	  for (index_feature *s = &l.sorted_features[0]; s <= &l.sorted_features.back();)
	  {
		  index_feature *next = s + 1;
		  while (next <= &l.sorted_features.back() && next->f.weight_index == s->f.weight_index)
			  next++;

		  //float *word_weights = &(weights[s->f.weight_index]);
		  weight_parameters::iterator word_weights = weights.change_begin() + (s->f.weight_index & weights.mask());
		  for (size_t k = 0; k < l.all->lda; k++, ++word_weights)
		  {
			  float new_value = minuseta * *word_weights;
			  *word_weights = new_value;
		  }

		  for (; s != next; s++)
		  {
			  float *v_s = &(l.v[s->document * l.all->lda]);
			  weight_parameters::iterator u_for_w = weights.change_begin() + (s->f.weight_index & weights.mask()) + l.all->lda + 1;
			  float c_w = eta * find_cw(l, u_for_w, v_s) * s->f.x;
			  word_weights = weights.change_begin() + (s->f.weight_index & weights.mask());
			  for (size_t k = 0; k < l.all->lda; k++, ++u_for_w, ++word_weights)
			  {   
				  float new_value = *u_for_w * v_s[k] * c_w;
				  l.total_new[k] += new_value;
				  *word_weights += new_value;
			  }
		  }
	  }
  
	  for (size_t k = 0; k < l.all->lda; k++)
	  { l.total_lambda[k] *= minuseta;
		l.total_lambda[k] += l.total_new[k];
	  }
  }
  l.sorted_features.resize(0);

  l.examples.erase();
  l.doc_lengths.erase();
}

void learn(lda &l, LEARNER::base_learner &, example &ec)
{ 
  uint32_t num_ex = (uint32_t)l.examples.size();
  l.examples.push_back(&ec);
  l.doc_lengths.push_back(0);
  for (features& fs : ec)
  { for (features::iterator& f : fs)
    { index_feature temp = {num_ex, feature(f.value(), f.index())};
      l.sorted_features.push_back(temp);
      l.doc_lengths[num_ex] += (int)f.value();
    }
  }
  if (++num_ex == l.minibatch)
    learn_batch(l);
}

void learn_with_metrics(lda &l, LEARNER::base_learner &base, example &ec)
{
  if (l.all->passes_complete == 0)
  { // build feature to example map
	auto weight_mask = l.all->weights.mask();
    auto stride_shift = l.all->weights.stride_shift();
    
    for (features& fs : ec)
    { for (features::iterator& f : fs)
      { uint64_t idx = (f.index() & weight_mask) >> stride_shift;
        l.feature_counts[idx] += (uint32_t)f.value();
        l.feature_to_example_map[idx].push_back(ec.example_counter);
      }
    }
  }

  learn(l, base, ec);
}

// placeholder
void predict(lda &l, LEARNER::base_learner &base, example &ec) { learn(l, base, ec); }
void predict_with_metrics(lda &l, LEARNER::base_learner &base, example &ec) { learn_with_metrics(l, base, ec); }

struct word_doc_frequency
{
	// feature/word index
	uint64_t idx;
	// document count
	uint32_t count;
};

// cooccurence of 2 features/words
struct feature_pair
{
	// feature/word 1
	uint64_t f1;
	// feature/word 2
	uint64_t f2;

	feature_pair(uint64_t _f1, uint64_t _f2) : f1(_f1), f2(_f2)
	{}
};

void get_top_weights(vw* all, int top_words_count, int topic, std::vector<feature>& output)
{
	weight_parameters& weights = all->weights;
	uint64_t length = (uint64_t)1 << all->num_bits;

	// get top features for this topic
	auto cmp = [](feature left, feature right) { return left.x > right.x; };
	std::priority_queue<feature, std::vector<feature>, decltype(cmp)> top_features(cmp);
	weight_parameters::iterator iter = weights.begin();
	for (uint64_t i = 0; i < min(top_words_count, length); i++, ++iter)
	  top_features.push({(&(*iter))[topic], i});

	for (uint64_t i = top_words_count; i < length; i++, ++iter)
	{  
		weight v = (&(*iter))[topic];
		if (v > top_features.top().x)
		{ top_features.pop();
		  top_features.push({v, i});
		}
	}

	// extract idx and sort descending
	output.resize(top_features.size());
	for (int i = (int)top_features.size() - 1; i >= 0; i--)
	{
		output[i] = top_features.top();
		top_features.pop();
	}
}

void compute_coherence_metrics(lda &l)
{
	weight_parameters& weights = l.all->weights;
	uint64_t length = (uint64_t)1 << l.all->num_bits;

	std::vector<std::vector<feature_pair>> topics_word_pairs;
	topics_word_pairs.resize(l.topics);

	int top_words_count = 10; // parameterize and check

 	// TODO: remove or make output file available through parameters
	/*
	FILE* vocab = fopen("C:\\Data\\MinMaxWordFreq_1200_0.3\\vocab.tsv", "w");
	for (int f = 0; f < length;f++)
	{
		fprintf(vocab, "%d\t%d\t%d\n", 
			f,
			l.feature_counts[f],
			l.feature_to_example_map[f].size());
	}
	fclose(vocab);

	FILE* docAlloc = fopen("C:\\Data\\MinMaxWordFreq_1200_0.3\\VW-DocumentTopicAllocations.txt", "w");
	// using jagged array to enable LINQ
	auto K = l.all->lda;

	uint64_t stride_shift = l.all->reg.stride_shift;
	for (uint64_t i = 0; i < length; i++)
	{
		auto offset = i << stride_shift;

		// over topics
		for (uint64_t k = 0; k < K; k++)
		{
			weight *v = &(l.all->reg.weight_vector[offset + k]);
			fprintf(docAlloc, "%f ", *v + l.lda_rho);
		}
		fprintf(docAlloc, "\n");
	}
	fclose(docAlloc);
	*/

	for (size_t topic = 0; topic < l.topics;topic++)
	{
		// get top features for this topic
		auto cmp = [](feature& left, feature& right) { return left.x > right.x; };
		std::priority_queue<feature, std::vector<feature>, decltype(cmp)> top_features(cmp);
		weight_parameters::iterator iter = weights.begin();
		for (uint64_t i = 0; i < min(top_words_count, length); i++, ++iter)
			top_features.push(feature((&(*iter))[topic], i));
		
		iter = weights.begin() + top_words_count;
		for (uint64_t i = top_words_count; i < length; i++, ++iter)
		{if ((&(*iter))[topic] > top_features.top().x)
			{  top_features.pop();
			   top_features.push(feature((&(*iter))[topic], i));
			}
		}

		// extract idx and sort descending
		vector<uint64_t> top_features_idx;
		top_features_idx.resize(top_features.size());
		for (int i = (int)top_features.size() - 1; i >= 0; i--)
		{
			top_features_idx[i] = top_features.top().weight_index;
			top_features.pop();
		}

		auto& word_pairs = topics_word_pairs[topic];
		for (size_t i = 0; i < top_features_idx.size(); i++)
			for (size_t j = i + 1; j < top_features_idx.size(); j++)
				word_pairs.push_back(feature_pair(top_features_idx[i], top_features_idx[j]));
	}

	// compress word pairs and create record for storing frequency
	std::map<uint64_t, std::vector<word_doc_frequency>> coWordsDFSet;
	for (auto& vec : topics_word_pairs)
	{
		for (auto& wp : vec)
		{
			auto f1 = wp.f1;
			auto f2 = wp.f2;
			auto wdf = coWordsDFSet.find(f1);

			if (wdf != coWordsDFSet.end())
			{
				// http://stackoverflow.com/questions/5377434/does-stdmapiterator-return-a-copy-of-value-or-a-value-itself
				//if (wdf->second.find(f2) == wdf->second.end())

				if (std::find_if(wdf->second.begin(), wdf->second.end(), [&f2](const word_doc_frequency& v) { return v.idx == f2; }) != wdf->second.end())
				{
					wdf->second.push_back({ f2, 0 });
					//printf(" add %d %d\n", f1, f2);
				}
			}
			else
			{
				std::vector<word_doc_frequency> vec = { { f2, 0 } };
				coWordsDFSet.insert(std::make_pair(f1, vec));
				//printf(" insert %d %d\n", f1, f2);
			}
		}
	}

	// this.GetWordPairsDocumentFrequency(coWordsDFSet);
	for (auto& pair : coWordsDFSet)
	{
		auto& examples_for_f1 = l.feature_to_example_map[pair.first];
		for (auto& wdf : pair.second)
		{
			auto& examples_for_f2 = l.feature_to_example_map[wdf.idx];

			// assumes examples_for_f1 and examples_for_f2 are orderd
			size_t i = 0;
			size_t j = 0;
			while (i < examples_for_f1.size() && j < examples_for_f2.size())
			{
				if (examples_for_f1[i] == examples_for_f2[j])
				{
					wdf.count++;
					i++;
					j++;
				}
				else if (examples_for_f2[j] < examples_for_f1[i])
					j++;
				else
					i++;
			}
		}
	}

	float epsilon = 1e-6f; // TODO
	float avg_coherence = 0;
	for (size_t topic = 0; topic < l.topics;topic++)
	{
		float coherence = 0;

		for (auto& pairs : topics_word_pairs[topic])
		{
			auto f1 = pairs.f1;
			if (l.feature_counts[f1] == 0)
				continue;

			auto f2 = pairs.f2;
			auto& co_feature = coWordsDFSet[f1];
			auto co_feature_df = std::find_if(co_feature.begin(), co_feature.end(), [&f2](const word_doc_frequency& v) { return v.idx == f2; });

			if (co_feature_df != co_feature.end())
			{
				// printf("(%d:%d + eps)/(%d:%d)\n", f2, co_feature_df->count, f1, l.feature_counts[f1]);
				coherence += logf((co_feature_df->count + epsilon) / l.feature_counts[f1]);
			}
		}

		printf("Topic %3d coherence: %f\n", (int)topic, coherence);

		// TODO: expose per topic coherence

		// TODO: good vs. bad topics
		avg_coherence += coherence;
	}

	avg_coherence /= l.topics;

	printf("Avg topic coherence: %f\n", avg_coherence);
}

void end_pass(lda &l)
{ 
	if (l.examples.size())
		learn_batch(l);

	if (l.compute_coherence_metrics && l.all->passes_complete == l.all->numpasses)
	{
		compute_coherence_metrics(l);
		// FASTPASS return;
	}
}

void end_examples(lda &l)
{  weight_parameters& weights = l.all->weights;
   for (weight_parameters::iterator iter = weights.begin(); iter != weights.end(); ++iter)
   { float decay_component =
      l.decay_levels.last() - l.decay_levels.end()[(int)(-1 - l.example_t + (&(*iter))[l.all->lda])];
    float decay = fmin(1.f, correctedExp(decay_component));

	for (weights_iterator_iterator<weight> k = iter.begin(); k != iter.end(l.all->lda); ++k)
      *k *= decay;
   }
}

void finish_example(vw&, lda&, example &) {}

void finish(lda &ld)
{ ld.sorted_features.~vector<index_feature>();
  ld.Elogtheta.delete_v();
  ld.decay_levels.delete_v();
  ld.total_new.delete_v();
  ld.examples.delete_v();
  ld.total_lambda.delete_v();
  ld.doc_lengths.delete_v();
  ld.digammas.delete_v();
  ld.v.delete_v();
}

std::istream &operator>>(std::istream &in, lda_math_mode &mmode)
{ using namespace boost::program_options;

  std::string token;
  in >> token;
  if (token == "simd")
    mmode = USE_SIMD;
  else if (token == "accuracy" || token == "precise")
    mmode = USE_PRECISE;
  else if (token == "fast-approx" || token == "approx")
    mmode = USE_FAST_APPROX;
  else
    throw boost::program_options::invalid_option_value(token);
  return in;
}

LEARNER::base_learner *lda_setup(vw &all)
{ if (missing_option<uint32_t, true>(all, "lda", "Run lda with <int> topics"))
    return nullptr;
  new_options(all, "Lda options")
    ("lda_alpha", po::value<float>()->default_value(0.1f),"Prior on sparsity of per-document topic weights")
    ("lda_rho", po::value<float>()->default_value(0.1f), "Prior on sparsity of topic distributions")
    ("lda_D", po::value<float>()->default_value(10000.), "Number of documents")
    ("lda_epsilon", po::value<float>()->default_value(0.001f), "Loop convergence threshold")
    ("minibatch", po::value<size_t>()->default_value(1), "Minibatch size, for LDA")
    ("math-mode", po::value<lda_math_mode>()->default_value(USE_SIMD), "Math mode: simd, accuracy, fast-approx")
    ("metrics", po::value<bool>()->default_value(false), "Compute metrics");
  add_options(all);
  po::variables_map &vm = all.vm;

  all.lda = vm["lda"].as<uint32_t>();
  all.delete_prediction = delete_scalars;

  lda &ld = calloc_or_throw<lda>();

  ld.topics = all.lda;
  ld.lda_alpha = vm["lda_alpha"].as<float>();
  ld.lda_rho = vm["lda_rho"].as<float>();
  ld.lda_D = vm["lda_D"].as<float>();
  ld.lda_epsilon = vm["lda_epsilon"].as<float>();
  ld.minibatch = vm["minibatch"].as<size_t>();
  ld.sorted_features = std::vector<index_feature>();
  ld.total_lambda_init = 0;
  ld.all = &all;
  ld.example_t = all.initial_t;
  ld.mmode = vm["math-mode"].as<lda_math_mode>();
  ld.compute_coherence_metrics = vm["metrics"].as<bool>();
  if (ld.compute_coherence_metrics) {
	  ld.feature_counts.resize((uint32_t)1 << all.num_bits);
	  ld.feature_to_example_map.resize((uint32_t)1 << all.num_bits);
  }

  float temp = ceilf(logf((float)(all.lda * 2 + 1)) / logf(2.f));
  all.weights.stride_shift((size_t)temp);
  all.random_weights = true;
  all.add_constant = false;

  if (all.eta > 1.)
  { std::cerr << "your learning rate is too high, setting it to 1" << std::endl;
    all.eta = min(all.eta, 1.f);
  }

  if (vm.count("minibatch"))
  { size_t minibatch2 = next_pow2(ld.minibatch);
    all.p->ring_size = all.p->ring_size > minibatch2 ? all.p->ring_size : minibatch2;
  }

  *all.file_options << " --lda_alpha " << ld.lda_alpha;
  *all.file_options << " --lda_rho " << ld.lda_rho;

  ld.v.resize(all.lda * ld.minibatch);

  ld.decay_levels.push_back(0.f);

  LEARNER::learner<lda> &l = init_learner(&ld, ld.compute_coherence_metrics ? learn_with_metrics : learn, 1 << all.weights.stride_shift(), prediction_type::scalars);
  l.set_predict(ld.compute_coherence_metrics ? predict_with_metrics : predict);
  l.set_save_load(save_load);
  l.set_finish_example(finish_example);
  l.set_end_examples(end_examples);
  l.set_end_pass(end_pass);
  l.set_finish(finish);

  return make_base(l);
}
