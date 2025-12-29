/*
 * Copyright (C) 2025 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <Poco/SHA2Engine.h>

#include <core/common/tools/logger.hpp>

#include "instance.hpp"

namespace aos::sm::launcher {

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

Instance::Instance(const InstanceInfo& instance)
    : mInstanceInfo(instance)
{
    GenerateInstanceID();

    LOG_DBG() << "Created instance" << Log::Field("instanceID", mInstanceID.c_str());
}

Error Instance::Start()
{
    return ErrorEnum::eNone;
}

Error Instance::Stop()
{
    return ErrorEnum::eNone;
}

/***********************************************************************************************************************
 * Private
 **********************************************************************************************************************/

void Instance::GenerateInstanceID()
{
    Poco::SHA2Engine engine;

    auto idStr = std::string(mInstanceInfo.mItemID.CStr()) + ":" + std::string(mInstanceInfo.mSubjectID.CStr()) + ":"
        + std::to_string(mInstanceInfo.mInstance);

    engine.update(idStr);

    mInstanceID = Poco::DigestEngine::digestToHex(engine.digest());
}

void Instance::GetStatus(InstanceStatus& status) const
{
    static_cast<InstanceIdent&>(status) = static_cast<const InstanceIdent&>(mInstanceInfo);
    status.mPreinstalled                = false;
    status.mRuntimeID                   = mInstanceInfo.mRuntimeID;
    status.mManifestDigest              = mInstanceInfo.mManifestDigest;
    status.mState                       = InstanceStateEnum::eActivating;
    status.mError                       = ErrorEnum::eNone;
}

} // namespace aos::sm::launcher
