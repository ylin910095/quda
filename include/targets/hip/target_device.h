#pragma once
#include <hip/hip_runtime.h>
#include <quda_api.h>
#include <algorithm>

namespace quda {

  namespace device {

    /**
       @brief Helper function that returns if the current execution
       region is on the device
    */
    constexpr bool is_device()
    {
#if defined(__HIP_DEVICE_COMPILE__)
      return true;
#else
      return false;
#endif
    }

    /**
       @brief Helper function that returns if the current execution
       region is on the host
    */
    constexpr bool is_host()
    {
#if defined(__HIP_DEVICE_COMPILE__)
      return false;
#else
      return true;
#endif
    }

    /**
       @brief Helper function that returns the thread block
       dimensions.  On CUDA this returns the intrinsic blockDim,
       whereas on the host this returns (1, 1, 1).
    */
    __device__ __host__ inline dim3 block_dim()
    {
#if defined(__HIP_DEVICE_COMPILE__)
      return dim3(blockDim.x, blockDim.y, blockDim.z);
#else
      return dim3(1, 1, 1);
#endif
    }

    /**
       @brief Helper function that returns the thread indices within a
       thread block.  On CUDA this returns the intrinsic
       blockIdx, whereas on the host this just returns (0, 0, 0).
    */
    __device__ __host__ inline dim3 block_idx()
    {
#if defined(__HIP_DEVICE_COMPILE__)
      return dim3(blockIdx.x, blockIdx.y, blockIdx.z);
#else
      return dim3(0, 0, 0);
#endif
    }

    /**
       @brief Helper function that returns the thread indices within a
       thread block.  On CUDA this returns the intrinsic
       threadIdx, whereas on the host this just returns (0, 0, 0).
    */
    __device__ __host__ inline dim3 thread_idx()
    {
#if defined(__HIP_DEVICE_COMPILE__)
      return dim3(threadIdx.x, threadIdx.y, threadIdx.z);
#else
      return dim3(0, 0, 0);
#endif
    }

    /**
       @brief Helper function that returns the warp-size of the
       architecture we are running on.
    */
    constexpr int warp_size() { return warpSize; }

    /**
       @brief Return the thread mask for a converged warp.
    */
    constexpr unsigned int warp_converged_mask() { return 0xffffffff; }

    /**
       @brief Helper function that returns the maximum number of threads
       in a block in the x dimension.
    */
    template <int block_size_y = 1, int block_size_z = 1>
      constexpr unsigned int max_block_size()
      {
        return std::max(warp_size(), 256 / (block_size_y * block_size_z));
      }

    /**
     * @brief Helper function for the transform reduce blocksize
     */
    constexpr unsigned int transform_reduce_block_size()
    {
          return 256;
    }

    /**
       @brief Helper function that returns the maximum number of threads
       in a block in the x dimension for reduction kernels.
    */
    template <int block_size_y = 1, int block_size_z = 1>
      constexpr unsigned int max_reduce_block_size()
      {
#ifdef QUDA_FAST_COMPILE_REDUCE
        // This is the specialized variant used when we have fast-compilation mode enabled
        return warp_size();
#else
        return max_block_size<block_size_y, block_size_z>();
#endif
      }

    /**
       @brief Helper function that returns the maximum number of threads
       in a block in the x dimension for reduction kernels.
    */
    constexpr unsigned int max_multi_reduce_block_size()
    {
#ifdef QUDA_FAST_COMPILE_REDUCE
      // This is the specialized variant used when we have fast-compilation mode enabled
      return warp_size();
#else
      return 128;
#endif
    }

    /**
       @brief Helper function that returns the maximum size of a
       constant_param_t buffer on the target architecture.  For CUDA,
       this corresponds to the maximum __constant__ buffer size.
    */
    constexpr size_t max_constant_param_size() { return 8192; }

    /**
       @brief Helper function that returns the maximum static size of
       the kernel arguments passed to a kernel on the target
       architecture.
    */
    constexpr size_t max_kernel_arg_size() { return 4096; }

    /**
       @brief Helper function that returns the bank width of the
       shared memory bank width on the target architecture.
    */
    constexpr int shared_memory_bank_width() { return 32; }

    /**
      @brief Return CUDA stream from QUDA stream.  This is a
      temporary addition until all kernels have been made generic.
      @param stream QUDA stream we which to convert to CUDA stream
      @return CUDA stream
    */
    hipStream_t get_cuda_stream(const qudaStream_t& stream);
  }

}
