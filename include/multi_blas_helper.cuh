#include <algorithm>
#include <register_traits.h>
#include <blas_helper.cuh>

namespace quda
{

  namespace blas
  {

    // storage for matrix coefficients
#define MAX_MATRIX_SIZE 8192
#define MAX_ARG_SIZE 4096
    __constant__ signed char Amatrix_d[MAX_MATRIX_SIZE];
    __constant__ signed char Bmatrix_d[MAX_MATRIX_SIZE];
    __constant__ signed char Cmatrix_d[MAX_MATRIX_SIZE];

    static signed char *Amatrix_h;
    static signed char *Bmatrix_h;
    static signed char *Cmatrix_h;

    /**
       @param[in] x Value we are testing
       @return True if x is a power of two
    */
    template <typename T> inline constexpr bool is_power2(T x) { return (x != 0) && ((x & (x - 1)) == 0); }

    /**
       @brief Return the maximum power of two enabled by default for
       multi-blas.  We set a lower limit for multi-reductions, since
       we can just transpose the inner product for free, and a high
       NXZ unroll for multi-reductions lead to poor performance due to
       register spilling.
       @param[in] reducer Whether we using a reducer
       @param[in] fixed Whether we are using fixed point
       @return Max power of two
    */
    inline int max_NXZ_power2(bool reducer, bool fixed = false) { return reducer ? 16 : (fixed ? 64 : 128); }

    /**
       @brief Return if the requested nxz parameter is valid or
       not.  E.g., a valid power of two, or is less than the the
       MAX_MULTI_BLAS_N parameter.
       @param[in] nxz Requested nxz parameter
       @return True if valid, false if not
     */
    inline bool is_valid_NXZ(int nxz, bool reducer, bool fixed = false)
    {
      if (nxz <= MAX_MULTI_BLAS_N || // all values below MAX_MULTI_BLAS_N are valid
          (is_power2(nxz) && nxz <= max_NXZ_power2(reducer, fixed))) {
        return true;
      } else {
        return false;
      }
    }

    /**
       @brief Helper function to compute the maximum YW size for the
       multi-blas runctions.  Since the SpinorX and SpinorZ arrays are
       statically allocated with length NXZ, we can statically compute how
       the maximum size of YW is and allocate this amount of space.  This
       allows for a much larger NXZ (NYW) when NYW (NXZ) is small.
    */
    template <int NXZ, typename SpinorX, typename SpinorY, typename SpinorZ, typename SpinorW, typename Functor>
    inline constexpr int max_YW_size()
    {
      // compute the size remaining for the Y and W accessors
      constexpr int arg_size = (MAX_ARG_SIZE - sizeof(int)                                    // NYW parameter
                                - sizeof(SpinorX[NXZ])                                        // SpinorX array
                                - (Functor::use_z ? sizeof(SpinorZ[NXZ]) : sizeof(SpinorZ *)) // SpinorZ array
                                - 2 * sizeof(int)                                             // functor NXZ/NYW members
                                - sizeof(int)                                                 // length parameter
                                - (!Functor::use_w ? sizeof(SpinorW *) : 0)   // subtract pointer if not using W
                                - (Functor::reducer ? sizeof(ReduceArg<void*>) : 0) // reduction buffers
                                - 16) // there seems to be 16 bytes other argument space we need
        / (sizeof(SpinorY) + (Functor::use_w ? sizeof(SpinorW) : 0));

      // this is the maximum size limit imposed by the coefficient arrays
      constexpr int coeff_size = Functor::coeff_mul ? MAX_MATRIX_SIZE / (NXZ * sizeof(typename Functor::coeff_t)) : arg_size;

      return std::min(arg_size, coeff_size);
    }

    /**
       @brief Helper function to compute the maximum YW size for the
       multi-blas runctions.  Since the SpinorX and SpinorZ arrays are
       statically allocated with length NXZ, we can statically compute how
       the maximum size of YW is and allocate this amount of space.  This
       allows for a much larger NXZ (NYW) when NYW (NXZ) is small.
    */
    template <int NXZ, typename xType, typename yType, typename Functor>
    inline constexpr int max_YW_size()
    {
      using SpinorX = Spinor<xType, 4>;
      using SpinorY = Spinor<yType, 4>;
      using SpinorZ = SpinorX;
      using SpinorW = Spinor<xType, 4>;
      return max_YW_size<NXZ, SpinorX, SpinorY, SpinorZ, SpinorW, Functor>();
    }

    /**
       @brief Helper function to compute the maximum YW size for the
       multi-blas runctions.  Since the SpinorX and SpinorZ arrays are
       statically allocated with length NXZ, we can statically compute how
       the maximum size of YW is and allocate this amount of space.  This
       allows for a much larger NXZ (NYW) when NYW (NXZ) is small.

       @param[in] scalar_width Width of the scalar that we're
       multiplying by (1 = real, 2 = complex)
    */
    inline int max_YW_size(int NXZ, QudaPrecision x_prec, QudaPrecision y_prec, bool use_z, bool use_w, int scalar_width, bool reduce)
    {
      bool x_fixed = x_prec < QUDA_SINGLE_PRECISION;
      bool y_fixed = y_prec < QUDA_SINGLE_PRECISION;
      size_t scalar_size = scalar_width * std::max(std::max(x_prec, y_prec), QUDA_SINGLE_PRECISION);
      NXZ = is_valid_NXZ(NXZ, reduce, x_fixed) ? NXZ : MAX_MULTI_BLAS_N; // ensure NXZ is a valid size
      size_t spinor_x_size = x_fixed ? sizeof(Spinor<short, 4>) : sizeof(Spinor<float, 4>);
      size_t spinor_y_size = y_fixed ? sizeof(Spinor<short, 4>) : sizeof(Spinor<float, 4>);

      size_t spinor_z_size = spinor_x_size;
      size_t spinor_w_size = x_fixed ? sizeof(Spinor<short, 4>) : sizeof(Spinor<float, 4>);

      // compute the size remaining for the Y and W accessors
      int arg_size = (MAX_ARG_SIZE - sizeof(int)                       // NYW parameter
                      - NXZ * spinor_x_size                            // SpinorX array
                      - (use_z ? NXZ * spinor_z_size : sizeof(void *)) // SpinorZ array (else dummy pointer)
                      - 2 * sizeof(int)                                    // functor NXZ/NYW members
                      - sizeof(int)                                    // length parameter
                      - (!use_w ? sizeof(void *) : 0)                  // subtract dummy pointer if not using W
                      - (reduce ? sizeof(ReduceArg<void*>) : 0)              // reduction buffers
                      - 16) // there seems to be 16 bytes other argument space we need
        / (spinor_y_size + (use_w ? spinor_w_size : 0));

      // this is the maximum size limit imposed by the coefficient arrays
      int coeff_size = scalar_width > 0 ? MAX_MATRIX_SIZE / (NXZ * scalar_size) : arg_size;

      return std::min(arg_size, coeff_size);
    }

    template <int NXZ, typename store_t, int N, bool> struct SpinorXZ {
      Spinor<store_t, N> X[NXZ];
      Spinor<store_t, N> *Z;
      SpinorXZ() : Z(X) {}
    };

    template <int NXZ, typename store_t, int N> struct SpinorXZ<NXZ, store_t, N, true> {
      Spinor<store_t, N> X[NXZ];
      Spinor<store_t, N> Z[NXZ];
    };

    template <int NYW, typename store_t, int N, bool> struct SpinorYW {
      Spinor<store_t, N> Y[NYW];
      Spinor<store_t, N> *W;
      SpinorYW() : W(Y) {}
    };

    template <int NYW, typename store_t, int N> struct SpinorYW<NYW, store_t, N, true> {
      Spinor<store_t, N> Y[NYW];
      Spinor<store_t, N> W[NYW];
    };

    template <typename T> struct coeff_array {
      using type = T;
      const T *data;
      coeff_array() : data(nullptr) {}
      coeff_array(const T *data) : data(data) {}
    };

    namespace detail
    {
      template <unsigned... digits> struct to_chars {
        static const char value[];
      };

      template <unsigned... digits> const char to_chars<digits...>::value[] = {('0' + digits)..., 0};

      template <unsigned rem, unsigned... digits> struct explode : explode<rem / 10, rem % 10, digits...> {
      };

      template <unsigned... digits> struct explode<0, digits...> : to_chars<digits...> {
      };
    } // namespace detail

    template <unsigned num> struct num_to_string : detail::explode<num / 10, num % 10> {
    };

  } // namespace blas

} // namespace quda
