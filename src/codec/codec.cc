/*
 * Copyright (c) 2023 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#include "vesal/codec.h"

#include <iostream>
#include <memory>
#include <utility>

#include "codec/codec_internal.h"
#include "codec/qat/qat_codec.h"
#include "codec/sw/sw_codec.h"
#include "vesal/status.h"

namespace vesal {

std::unique_ptr<qat::QatCodec> g_qat_codec = nullptr;
std::unique_ptr<sw::SwCodec> g_sw_codec = nullptr;

bool Codec::Init(const CodecInitOptions& init_opts) {
    g_sw_codec = std::make_unique<sw::SwCodec>();
    if (init_opts.init_qat) {
        auto qat_codec = std::make_unique<qat::QatCodec>();
        Status qat_r = qat_codec->Start();
        if (!qat_r.ok()) {
            VESAL_LOG(WARN) << "Failed to initialize QAT Codec engine: " << qat_r.message();
            return false;
        }
        g_qat_codec = std::move(qat_codec);
    }
    return true;
}

bool Codec::Uninit() {
    if (g_qat_codec) {
        auto r = g_qat_codec->Stop();
        if (!r.ok()) {
            return false;
        }
        g_qat_codec.reset();
    }
    if (g_sw_codec) {
        g_sw_codec.reset();
    }
    return true;
}

std::pair<Status, std::unique_ptr<CodecChannel>> CodecChannel::CreateCodecChannel(
    const CodecChannelOption& opts) {
    switch (opts.engine_type) {
    case CodecEngineType::kQat:
        if (!g_qat_codec) {
            return std::make_pair(NotSupportedError("Qat engine is not initialized"), nullptr);
        }
        return g_qat_codec->CreateCodecChannel(opts);
    case CodecEngineType::kSoftware:
        if (!g_sw_codec) {
            return std::make_pair(NotSupportedError("Software engine is not initialized"), nullptr);
        }
        return g_sw_codec->CreateCodecChannel(opts);
    default:
        break;
    }
    return std::make_pair(NotSupportedError("Wrong engine type"), nullptr);
}

std::ostream& operator<<(std::ostream& os, const CodecChannelOption& opt) {
    return os << "comp_algorithm=" << static_cast<int>(opt.comp_algorithm)
              << ", comp_level=" << static_cast<int>(opt.comp_level)
              << ", checksum_type=" << static_cast<int>(opt.checksum_type)
              << ", compressed_checksum=" << opt.compressed_checksum
              << ", allocation_option.node_affinity=" << opt.allocation_option.node_affinity
              << ", poll_mode=" << static_cast<int>(opt.poll_mode)
              << ", timeout_ms=" << opt.timeout_ms;
}

bool CodecChannelOption::operator<(const CodecChannelOption& rhs) const {
    return std::tie(user_cb,
                    ha_policy,
                    comp_algorithm,
                    comp_level,
                    checksum_type,
                    compressed_checksum,
                    allocation_option.node_affinity,
                    sw_backup,
                    poll_mode,
                    timeout_ms) < std::tie(rhs.user_cb,
                                          rhs.ha_policy,
                                          rhs.comp_algorithm,
                                          rhs.comp_level,
                                          rhs.checksum_type,
                                          rhs.compressed_checksum,
                                          rhs.allocation_option.node_affinity,
                                          rhs.sw_backup,
                                          rhs.poll_mode,
                                          rhs.timeout_ms);
}

std::ostream& operator<<(std::ostream& os, const CodecResult& res) {
    bool need_reset_hex = (os.flags() ^ std::ios_base::hex) != 0;
    os << "consumed: " << res.consumed << ", produced: " << res.produced
       << ", in_checksum: " << res.in_checksum << ", out_checksum: " << res.out_checksum
       << ", status: " << res.status << std::hex
       << ", ctx: " << reinterpret_cast<uintptr_t>(res.ctx);
    // reset modified hex flag
    if (need_reset_hex) {
        os.flags(os.flags() ^ std::ios_base::hex);
    }
    return os;
}

}  // namespace vesal