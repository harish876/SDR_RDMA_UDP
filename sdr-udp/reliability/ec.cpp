#include "reliability/ec.h"
#include <iostream>
#ifdef HAS_ISAL
#include <isa-l/erasure_code.h>
#endif

namespace sdr::reliability {

int ECSender::encode_and_send(SDRConnection* conn, const void* buffer, size_t length) {
    conn_ = conn;
    sends_.clear();
    const uint64_t data_bytes = cfg_.data_bytes ? cfg_.data_bytes : length;
    const uint16_t k = cfg_.k_data ? cfg_.k_data : 4;
    const uint16_t m = cfg_.m_parity ? cfg_.m_parity : 2;
    const ConnectionParams& params = conn->connection_ctx->get_params();
    const uint32_t chunk_bytes = params.mtu_bytes * params.packets_per_chunk;
    if (chunk_bytes == 0) {
        std::cerr << "[EC] Invalid chunk_bytes\n";
        return -1;
    }
    uint32_t data_chunks = static_cast<uint32_t>((data_bytes + chunk_bytes - 1) / chunk_bytes);
    uint32_t stripes = (data_chunks + k - 1) / k;
    uint32_t parity_chunks = stripes * m;
    uint64_t total_chunks = data_chunks + parity_chunks;
    uint64_t total_bytes = total_chunks * chunk_bytes;

    std::vector<uint8_t> send_buf(total_bytes, 0);
    std::memcpy(send_buf.data(), buffer, std::min<uint64_t>(data_bytes, total_bytes));

#ifdef HAS_ISAL
    std::vector<uint8_t*> data_ptrs(k);
    std::vector<uint8_t*> parity_ptrs(m);
    std::vector<uint8_t> encode_matrix(k * (k + m));
    std::vector<uint8_t> gftbl(m * k * 32);
    gf_gen_rs_matrix(encode_matrix.data(), k + m, k);
    ec_init_tables(k, m, encode_matrix.data(), gftbl.data());

    for (uint32_t s = 0; s < stripes; ++s) {
        uint32_t stripe_data = std::min<uint32_t>(k, data_chunks - s * k);
        for (uint32_t i = 0; i < stripe_data; ++i) {
            data_ptrs[i] = send_buf.data() + (s * k + i) * chunk_bytes;
        }
        for (uint32_t p = 0; p < m; ++p) {
            parity_ptrs[p] = send_buf.data() + (data_chunks + s * m + p) * chunk_bytes;
        }
        ec_encode_data(chunk_bytes, stripe_data, m, gftbl.data(), data_ptrs.data(), parity_ptrs.data());
    }
#endif

    SDRSendHandle* raw_handle = nullptr;
    int rc = sdr_send_post(conn, send_buf.data(), send_buf.size(), &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to send data+parity buffer\n";
        return rc;
    }
    sends_.emplace_back(raw_handle, [](SDRSendHandle* h){ delete h; });
    return 0;
}

int ECSender::poll() {
    int rc = 0;
    for (auto& h : sends_) {
        rc = sdr_send_poll(h.get());
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

int ECReceiver::post_receive(SDRConnection* conn, void* buffer, size_t length) {
    conn_ = conn;
    data_bytes_ = cfg_.data_bytes ? cfg_.data_bytes : length;
    k_ = cfg_.k_data ? cfg_.k_data : 4;
    m_ = cfg_.m_parity ? cfg_.m_parity : 2;
    chunk_bytes_ = conn->connection_ctx->get_params().mtu_bytes * conn->connection_ctx->get_params().packets_per_chunk;
    if (chunk_bytes_ == 0) chunk_bytes_ = 1;
    data_chunks_ = static_cast<uint32_t>((data_bytes_ + chunk_bytes_ - 1) / chunk_bytes_);
    stripes_ = (data_chunks_ + k_ - 1) / k_;
    parity_chunks_ = stripes_ * m_;
    SDRRecvHandle* raw_handle = nullptr;
    int rc = sdr_recv_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to post receive\n";
        return rc;
    }
    recv_handle_.reset(raw_handle);
    return 0;
}

bool ECReceiver::try_decode() {
    if (!recv_handle_) {
        return false;
    }
    // Check bitmap for missing data chunks
    const uint8_t* bitmap = nullptr;
    size_t len = 0;
    if (sdr_recv_bitmap_get(recv_handle_.get(), &bitmap, &len) != 0) {
        return false;
    }
    auto* ctx = recv_handle_->msg_ctx.get();
    if (!ctx || !ctx->frontend_bitmap) return false;
    uint32_t missing_data = 0;
    for (uint32_t c = 0; c < data_chunks_; ++c) {
        if (!ctx->frontend_bitmap->is_chunk_complete(c)) {
            missing_data++;
        }
    }
    if (missing_data == 0) {
        stats_.decode_success++;
        return true;
    }
    uint32_t avail_parity = parity_chunks_;
    if (missing_data > avail_parity) {
        stats_.fallback_sr++;
        return false; // trigger SR fallback
    }

#ifdef HAS_ISAL
    std::vector<uint8_t*> data_ptrs(k_);
    std::vector<uint8_t*> parity_ptrs(m_);
    std::vector<uint8_t> encode_matrix((k_ + m_) * k_);
    std::vector<uint8_t> decode_matrix(k_ * k_);
    std::vector<uint8_t> gftbl(m_ * k_ * 32);
    gf_gen_rs_matrix(encode_matrix.data(), k_ + m_, k_);

    // Build lists of available chunks and missing chunks
    std::vector<uint32_t> missing_idxs;
    std::vector<uint32_t> avail_idxs;
    for (uint32_t c = 0; c < data_chunks_ + parity_chunks_; ++c) {
        if (c < data_chunks_) {
            if (!ctx->frontend_bitmap->is_chunk_complete(c)) {
                missing_idxs.push_back(c);
            } else {
                avail_idxs.push_back(c);
            }
        } else {
            if (ctx->frontend_bitmap->is_chunk_complete(c)) {
                avail_idxs.push_back(c);
            }
        }
    }
    if (missing_idxs.size() > m_) {
        stats_.fallback_sr++;
        return false;
    }

    // Set up decode matrix
    for (uint32_t i = 0; i < k_; ++i) {
        for (uint32_t j = 0; j < k_; ++j) {
            decode_matrix[i * k_ + j] = encode_matrix[avail_idxs[i] * k_ + j];
        }
    }
    if (gf_invert_matrix(decode_matrix.data(), gftbl.data(), k_) < 0) {
        stats_.fallback_sr++;
        return false;
    }
    // Prepare pointers
    for (uint32_t i = 0; i < k_; ++i) {
        data_ptrs[i] = static_cast<uint8_t*>(ctx->buffer) + avail_idxs[i] * chunk_bytes_;
    }
    for (size_t mi = 0; mi < missing_idxs.size(); ++mi) {
        uint32_t idx = missing_idxs[mi];
        parity_ptrs[mi] = static_cast<uint8_t*>(ctx->buffer) + idx * chunk_bytes_;
        ec_init_tables(k_, 1, decode_matrix.data() + mi * k_, gftbl.data());
        ec_encode_data(chunk_bytes_, k_, 1, gftbl.data(), data_ptrs.data(), &parity_ptrs[mi]);
    }
    stats_.decode_success++;
    return true;
#else
    stats_.fallback_sr++;
    return false;
#endif
}

} // namespace sdr::reliability
