#include "mds_ec.h"

#if __has_include(<isa-l/erasure_code.h>)
  #include <isa-l/erasure_code.h>
#elif __has_include(<erasure_code.h>)
  #include <erasure_code.h>
#else
  #error "Cannot find erasure_code.h — install libisal-dev."
#endif

#if __has_include(<isa-l.h>)
  #include <isa-l.h>
#elif __has_include(<isa-l/isa-l.h>)
  #include <isa-l/isa-l.h>
#endif

#include <stdexcept>
#include <cstring>
#include <iostream>

namespace {
  unsigned char gftbls[EC_PARITY_CHUNKS_M * EC_DATA_CHUNKS_K * 32];
  unsigned char encode_matrix[EC_GROUP_SIZE * EC_DATA_CHUNKS_K];
}

// ----------------------------------------------------------------------

namespace MDSErasureCoding {

void init() {
  std::cout << "[MDS] Initializing Reed–Solomon (k=" << EC_DATA_CHUNKS_K
            << ", m=" << EC_PARITY_CHUNKS_M << ")..." << std::endl;

  gf_gen_rs_matrix(encode_matrix, EC_GROUP_SIZE, EC_DATA_CHUNKS_K);
  ec_init_tables(EC_DATA_CHUNKS_K, EC_PARITY_CHUNKS_M,
                 &encode_matrix[EC_DATA_CHUNKS_K * EC_DATA_CHUNKS_K], gftbls);

  std::cout << "[MDS] Encoding tables ready." << std::endl;
}

// ----------------------------------------------------------------------

void encode(const PacketGroup& data_packets, PacketGroup& parity_packets) {
  if ((int)data_packets.size() != EC_DATA_CHUNKS_K ||
      (int)parity_packets.size() != EC_PARITY_CHUNKS_M)
    throw std::runtime_error("encode(): wrong group sizes");

  unsigned char* data_ptrs[EC_DATA_CHUNKS_K];
  unsigned char* parity_ptrs[EC_PARITY_CHUNKS_M];

  for (int i = 0; i < EC_DATA_CHUNKS_K; ++i)
    data_ptrs[i] = (unsigned char*)data_packets[i].payload;

  for (int i = 0; i < EC_PARITY_CHUNKS_M; ++i) {
    std::memset(parity_packets[i].payload, 0, CHUNK_PAYLOAD_SIZE);
    parity_ptrs[i] = (unsigned char*)parity_packets[i].payload;
  }

  ec_encode_data(CHUNK_PAYLOAD_SIZE, EC_DATA_CHUNKS_K, EC_PARITY_CHUNKS_M,
                 gftbls, data_ptrs, parity_ptrs);
}

// ----------------------------------------------------------------------
//  Decode version that avoids gf_mul_matrix
// ----------------------------------------------------------------------

bool decode(PacketGroup& received_packets) {
  unsigned char erasures_idx[EC_GROUP_SIZE];
  unsigned char survivor_idx[EC_GROUP_SIZE];
  int num_erasures = 0;
  int survivors_for_decode = 0;

  for (int i = 0; i < EC_GROUP_SIZE; ++i) {
    if (received_packets[i].data_size == 0)
      erasures_idx[num_erasures++] = (unsigned char)i;
    else if (survivors_for_decode < EC_DATA_CHUNKS_K)
      survivor_idx[survivors_for_decode++] = (unsigned char)i;
  }

  if (num_erasures == 0) return true;
  if (num_erasures > EC_PARITY_CHUNKS_M) return false;
  if (survivors_for_decode < EC_DATA_CHUNKS_K) return false;

  // Build survivor matrix
  unsigned char surv_mat[EC_DATA_CHUNKS_K * EC_DATA_CHUNKS_K];
  for (int r = 0; r < EC_DATA_CHUNKS_K; ++r)
    std::memcpy(&surv_mat[r * EC_DATA_CHUNKS_K],
                &encode_matrix[survivor_idx[r] * EC_DATA_CHUNKS_K],
                EC_DATA_CHUNKS_K);

  // Invert
  unsigned char inv_mat[EC_DATA_CHUNKS_K * EC_DATA_CHUNKS_K];
  if (gf_invert_matrix(surv_mat, inv_mat, EC_DATA_CHUNKS_K) < 0) {
    std::cerr << "[MDS] gf_invert_matrix failed.\n";
    return false;
  }

  // Build decode_rows = encode_rows(missing) × inv_mat
  unsigned char decode_rows[EC_PARITY_CHUNKS_M * EC_DATA_CHUNKS_K];
  for (int e = 0; e < num_erasures; ++e) {
    for (int c = 0; c < EC_DATA_CHUNKS_K; ++c) {
      unsigned char acc = 0;
      for (int j = 0; j < EC_DATA_CHUNKS_K; ++j) {
        unsigned char a = encode_matrix[erasures_idx[e] * EC_DATA_CHUNKS_K + j];
        unsigned char b = inv_mat[j * EC_DATA_CHUNKS_K + c];
        acc ^= gf_mul(a, b);
      }
      decode_rows[e * EC_DATA_CHUNKS_K + c] = acc;
    }
  }

  // Init decode tables
  unsigned char decode_gftbls[EC_DATA_CHUNKS_K * EC_DATA_CHUNKS_K * 32];
  ec_init_tables(EC_DATA_CHUNKS_K, num_erasures, decode_rows, decode_gftbls);

  unsigned char* in_ptrs[EC_DATA_CHUNKS_K];
  for (int i = 0; i < EC_DATA_CHUNKS_K; ++i)
    in_ptrs[i] = (unsigned char*)received_packets[survivor_idx[i]].payload;

  unsigned char* out_ptrs[EC_PARITY_CHUNKS_M];
  for (int i = 0; i < num_erasures; ++i) {
    int idx = erasures_idx[i];
    out_ptrs[i] = (unsigned char*)received_packets[idx].payload;
    std::memset(out_ptrs[i], 0, CHUNK_PAYLOAD_SIZE);
  }

  ec_encode_data(CHUNK_PAYLOAD_SIZE, EC_DATA_CHUNKS_K, num_erasures,
                 decode_gftbls, in_ptrs, out_ptrs);

  for (int i = 0; i < num_erasures; ++i) {
    int idx = erasures_idx[i];
    received_packets[idx].data_size = CHUNK_PAYLOAD_SIZE;
    if (idx < EC_DATA_CHUNKS_K) {
      received_packets[idx].type = DATA_CHUNK;
      received_packets[idx].chunk_index = idx;
    } else {
      received_packets[idx].type = PARITY_CHUNK;
      received_packets[idx].chunk_index = idx - EC_DATA_CHUNKS_K;
    }
  }
  return true;
}

} // namespace MDSErasureCoding
