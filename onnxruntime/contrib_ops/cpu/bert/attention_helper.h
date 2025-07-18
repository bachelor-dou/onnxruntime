// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <limits>
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"
#include "core/common/safeint.h"
#include "core/platform/threadpool.h"
#include "core/providers/common.h"
#include "core/mlas/inc/mlas.h"

using onnxruntime::concurrency::ThreadPool;

namespace onnxruntime {
namespace contrib {

template <typename T>
inline void ComputeSmoothSoftmaxInplace(T* score, int D, float sink, ThreadPool* tp) {
  MlasComputeSoftmax(score, score, 1, D, false, true, sink, tp);
}

template <typename T>
inline void ComputeAttentionSoftmaxInplace(T* score, int N, int D, ThreadPool* tp) {
  MlasComputeSoftmax(score, score, N, D, false, false, 0.0f, tp);
}

template <typename T>
void ComputeAttentionSoftcapInplace(T* scores, int sequence_length, T softcap) {
  MlasComputeSoftcap(scores, scores, sequence_length, softcap);
}

template <typename T>
void ApplyAttentionBias(T* softmax_logits, const T* attention_mask, int N) {
  MlasEltwiseAdd(softmax_logits, attention_mask, softmax_logits, N);
}

template <typename T>
void PrepareMask(const int32_t* mask_index,
                 gsl::span<const int64_t> mask_index_dims,
                 T* mask_data,
                 bool causal,
                 int batch_size,
                 int sequence_length,
                 int kv_sequence_length,
                 int past_sequence_length,
                 float mask_filter_value) {
  const int all_sequence_length = past_sequence_length + kv_sequence_length;

  // mask_data has been filled with 0, and its shape is BxSxT
  T* p_mask = mask_data;

  // 4D mask in Megatron GPT2 is currently not support in CPU kernel
  if (nullptr != mask_index && mask_index_dims.size() == 4) {
    ORT_NOT_IMPLEMENTED("4D mask in attention cpu kernel is not supported");
  }

  // For 3D mask, convert values 0 to mask_filter_value, and 1 to 0.0, then apply unidirectional mask if any.
  if (nullptr != mask_index && mask_index_dims.size() == 3) {
    for (int i = 0; i < batch_size * sequence_length * all_sequence_length; i++) {
      p_mask[i] = (mask_index[i] > 0) ? static_cast<T>(0.0f) : static_cast<T>(mask_filter_value);
    }

    if (causal) {
      for (int b_i = 0; b_i < batch_size; b_i++) {
        for (int s_i = 0; s_i < sequence_length - 1; s_i++) {
          for (int m_i = past_sequence_length + s_i + 1; m_i < all_sequence_length; m_i++) {
            p_mask[s_i * all_sequence_length + m_i] = std::numeric_limits<T>::lowest();
          }
        }
        p_mask += static_cast<size_t>(sequence_length) * all_sequence_length;
      }
    }

    return;
  }

  bool is_raw_attention_mask = (nullptr != mask_index && mask_index_dims.size() == 2);
  bool has_mask_start_position = (nullptr != mask_index &&
                                  mask_index_dims.size() == 1 &&
                                  static_cast<int>(mask_index_dims[0]) == 2 * batch_size);

  for (int b_i = 0; b_i < batch_size; b_i++) {
    // TODO: mask_index can be used in softmax to save some calculation.
    if (nullptr != mask_index) {
      if (is_raw_attention_mask) {
        // Raw attention mask has value 0 or 1. Here we convert 0 to mask_filter_value, and 1 to 0.0.
        ptrdiff_t off = SafeInt<ptrdiff_t>(b_i) * all_sequence_length;
        const int32_t* raw_mask = mask_index + off;
        for (int m_i = 0; m_i < all_sequence_length; m_i++) {
          p_mask[m_i] = (raw_mask[m_i] > 0) ? static_cast<T>(0.0f) : static_cast<T>(mask_filter_value);
        }
      } else {
        // mask_index is 1D: (B) or (2B) => (Bx)T

        // Handle right-side padding: mask value at or after the end position will be mask_filter_value
        int end_position = mask_index[b_i];
        for (int m_i = end_position; m_i < all_sequence_length; m_i++) {
          p_mask[m_i] = static_cast<T>(mask_filter_value);
        }

        // Handle left-side padding: mask value before the start position will be mask_filter_value
        if (has_mask_start_position) {
          int start_position = std::min(mask_index[b_i + batch_size], all_sequence_length);
          for (int m_i = 0; m_i < start_position; m_i++) {
            p_mask[m_i] = static_cast<T>(mask_filter_value);
          }
        }
      }
    }

    // Broadcast mask from (Bx)T to (Bx)SxT
    for (ptrdiff_t s_i = 1; s_i < sequence_length; s_i++) {
      memcpy(p_mask + s_i * all_sequence_length, p_mask, all_sequence_length * sizeof(T));
    }

    // Apply unidirectional mask.
    if (causal) {
      for (int s_i = 0; s_i < sequence_length - 1; s_i++) {
        for (int m_i = past_sequence_length + s_i + 1; m_i < all_sequence_length; m_i++) {
          p_mask[s_i * all_sequence_length + m_i] = std::numeric_limits<T>::lowest();
        }
      }
    }

    ptrdiff_t mask_to_advance = SafeInt<ptrdiff_t>(sequence_length) * all_sequence_length;
    p_mask += mask_to_advance;
  }
}

// Concatenate a past state chunk PxH with input state chunk LxH into present state chunk TxH
// Returns a pointer to the start of present state chunk.
template <typename T>
T* ConcatStateChunk(const T* past,
                    const T* chunk,
                    T* present,
                    size_t past_chunk_length,
                    size_t present_chunk_length,
                    std::ptrdiff_t i) {
  T* start = present + i * present_chunk_length;

  T* p = start;
  if (nullptr != past) {
    const T* src_past = past + i * past_chunk_length;
    memcpy(p, src_past, past_chunk_length * sizeof(T));
    p += past_chunk_length;
  }

  memcpy(p, chunk, (present_chunk_length - past_chunk_length) * sizeof(T));
  return start;
}

// GQA version of ConcatStateChunk
template <typename T>
T* ConcatStateChunkGQA(const T* past,
                       const T* chunk,
                       T* present,
                       size_t present_buff_chunk_length,
                       size_t past_buff_chunk_length,
                       size_t past_chunk_length,
                       size_t new_chunk_length,
                       bool past_present_share_buffer,
                       std::ptrdiff_t i) {
  T* start = present + i * present_buff_chunk_length;

  T* p = start;
  if (!past_present_share_buffer && past_chunk_length > 0) {
    const T* src_past = past + i * past_buff_chunk_length;
    memcpy(p, src_past, past_chunk_length * sizeof(T));
  }
  p += past_chunk_length;

  memcpy(p, chunk, new_chunk_length * sizeof(T));
  return start;
}

}  // namespace contrib
}  // namespace onnxruntime
