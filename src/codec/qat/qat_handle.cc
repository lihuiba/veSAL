/*
 * Copyright (c) 2024 ByteDance Inc.
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

#include "codec/qat/qat_handle.h"

#include <functional>

#include "codec/qat/qat_error_handling.h"

namespace vesal {
namespace qat {

Status QatHandle::Init() {
    QatUnitSelection selection;
    selection.numa_id = channel_opts_.allocation_option.node_affinity;
    selection.pf_id = channel_opts_.allocation_option.device_id;
    selection.poll_mode = channel_opts_.poll_mode;
    unit_ = unit_manager_->GrabAvailableUnit(selection);
    if (unit_ == nullptr) {
        return ResourceBusyError("No available QAT unit.");
    }
    VESAL_LOG(INFO) << "Grabed " << *unit_;
    session_ = std::make_unique<QatSession>(unit_);
    Status sess_r = session_->Init(QatSessionOption(channel_opts_), Callback);
    if (!sess_r.ok()) {
        Uninit();
        return sess_r;
    }
    return OkStatus();
}

Status QatHandle::Uninit() {
    // Try close session
    if (session_) {
        StatusCode r = TryCloseSession();
        if (!IsOk(r)) {
            // Something wrong with closing session. For now we follow the old semantic and not
            // allow to close.
            // TODO(sjj): Change the behaviour, handle the error this layer because user can do
            // nothing.
            VESAL_LOG(WARN)
                << "Fail to close session, usually due to flying requests not cleared, r=" << r;
            return {r, "Fail to close session."};
        }
        // It's safe to delete the session here. Because if the session is not closed properly,
        // closing the Unit will handle all the stuff, QAT Driver gurantees that.
        session_.reset();
    }
    for (auto* sess : discarded_sessions_) {
        // For these hanging sessions we just try to close them and avoid calling the callback.
        sess->Close();
        delete sess;
    }
    discarded_sessions_.clear();
    if (unit_ != nullptr) {
        // We can safely put the unit back to the pool despite whether the session is
        // closed. Because a unit can bind to multiple sessions, there will be no conflict as long
        // as we don't use the unit in multithreads.
        unit_manager_->PutBackUnit(unit_);
        unit_ = nullptr;
    }
    for (auto* unit : discarded_units_) {
        unit_manager_->PutBackToBlackList(unit);
    }
    discarded_units_.clear();
    return OkStatus();
}

StatusCode QatHandle::Reinit() {
    // Don't close session as it might be hanging.
    discarded_sessions_.push_back(session_.release());
    discarded_units_.push_back(unit_);
    unit_ = nullptr;
    QatSessionOption sess_opts(channel_opts_);
    do {
        unit_ = unit_manager_->GrabFromDiffDevice(discarded_units_, channel_opts_.poll_mode);
        if (unit_ != nullptr) {
            session_ = std::make_unique<QatSession>(unit_);
            Status sess_init_r = session_->Init(sess_opts, Callback);
            // TODO(sjj): Might need more delicated handling here, i.e do different things for
            // different error types. For example, if the error is due to memory inefficiency, retry
            // might not be useful. But at this point we don't know much detail of QAT driver so
            // it's hard to tell whether the error is really memory inefficiency or not. Even we
            // can, we still don't know what to do. So just try a different instance and give it a
            // shot.
            if (sess_init_r.ok()) {
                return StatusCode::kOk;
            }
            VESAL_LOG(WARN) << "Fail to init session, r=" << sess_init_r
                            << " for QatUnit: " << *unit_;
            discarded_units_.push_back(unit_);
            discarded_sessions_.push_back(session_.release());
        }
    } while (unit_ != nullptr);
    return StatusCode::kHardwareError;
}

StatusCode QatHandle::SubmitAsync(RequestCbContext* cb_ctx, CodecDirection dir) {
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    CpaDcSessionHandle sess_hdl = session_->GetSessionHandle();
    CpaBufferList* src_buff_list = cb_ctx->src_qat->GetCpaBufferList();
    CpaBufferList* dst_buff_list = cb_ctx->dst_qat->GetCpaBufferList();
    CpaDcOpData* op_data = &cb_ctx->op_data;
    CpaDcRqResults* results = &cb_ctx->cpa_results;
    auto* api_wrapper = GetQatApiWrapper();
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
    auto api = dir == CodecDirection::kComp ? &QatHardwareApiWrapper::QAT_cpaDcCompressData2
                                            : &QatHardwareApiWrapper::QAT_cpaDcDecompressData2;
#ifdef VESAL_ENABLE_ERR_SIM
    StatusCode vesal_status = StatusCode::kOk;
    if (FLAGS_vesal_enable_err_sim) {
        QatErrSimCode code = unit_->GetQatErrSim(QatErrSimType::kSubmit).first;
        vesal_status = QatErrSimCodeToVesalStatusCode(code);
    }
    if (vesal_status != StatusCode::kOk) {
        // Fast return if we injected a vesal layer error.
        return vesal_status;
    }
#endif
    cpa_status = (api_wrapper->*api)(
        *inst_hdl, sess_hdl, src_buff_list, dst_buff_list, op_data, results, cb_ctx);
    return CpaStatusToVesalStatusCode(cpa_status);
}
// quoto = 0 means no quota limitation. Poll as much as possible.
StatusCode QatHandle::PollInstance(int quota) {
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
#ifdef VESAL_ENABLE_ERR_SIM
    StatusCode vesal_status = StatusCode::kOk;
    if (FLAGS_vesal_enable_err_sim) {
        QatErrSimCode code = unit_->GetQatErrSim(QatErrSimType::kPoll).first;
        vesal_status = QatErrSimCodeToVesalStatusCode(code);
    }
    if (vesal_status != StatusCode::kOk) {
        // Fast return if we injected a vesal layer error.
        return vesal_status;
    }
#endif
    cpa_status = GetQatApiWrapper()->QAT_icp_sal_DcPollInstance(*inst_hdl, 0);
    return CpaStatusToVesalStatusCode(cpa_status);
}

StatusCode QatHandle::TryCloseSession() {
    int cnt = 10;
    StatusCode ret = session_->Close().code();
    while (!IsOk(ret) && cnt-- > 0) {
        usleep(100);
        PollInstance();
        ret = session_->Close().code();
    }
    return ret;
}

}  // namespace qat
}  // namespace vesal