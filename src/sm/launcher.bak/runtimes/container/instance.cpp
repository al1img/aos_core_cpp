/*
 * Copyright (C) 2025 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <filesystem>
#include <numeric>

#include <Poco/SHA2Engine.h>

#include <core/common/tools/logger.hpp>

#include "instance.hpp"
#include "runtimeconfig.hpp"

namespace fs = std::filesystem;

namespace aos::sm::launcher {

namespace {

/***********************************************************************************************************************
 * Consts
 **********************************************************************************************************************/

static const char* const cBindEtcEntries[] = {"nsswitch.conf", "ssl"};

/***********************************************************************************************************************
 * Static
 **********************************************************************************************************************/

std::string JoinPath(const std::string& path1, const std::string& path2)
{
    fs::path fullPath = fs::path(path1) / fs::path(path2);

    return fullPath.string();
}

} // namespace

/***********************************************************************************************************************
 * Public
 **********************************************************************************************************************/

Instance::Instance(const InstanceInfo& instance, const ContainerConfig& config, const NodeInfo& nodeInfo,
    FileSystemItf& fileSystem, imagemanager::ItemInfoProviderItf& itemInfoProvider,
    networkmanager::NetworkManagerItf& networkManager, iamclient::PermHandlerItf& permHandler, oci::OCISpecItf& ociSpec)
    : mInstanceInfo(instance)
    , mConfig(config)
    , mNodeInfo(nodeInfo)
    , mFileSystem(fileSystem)
    , mItemInfoProvider(itemInfoProvider)
    , mNetworkManager(networkManager)
    , mPermHandler(permHandler)
    , mOCISpec(ociSpec)
{
    GenerateInstanceID();

    LOG_DBG() << "Created instance" << Log::Field("instanceID", mInstanceID.c_str());
}

Error Instance::Start()
{
    auto runtimeDir = JoinPath(mConfig.mRuntimeDir, mInstanceID);

    if (auto err = mFileSystem.ClearDir(runtimeDir); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    auto imageConfig   = std::make_unique<oci::ImageConfig>();
    auto serviceConfig = std::make_unique<oci::ServiceConfig>();
    auto runtimeConfig = std::make_unique<oci::RuntimeConfig>();

    if (auto err = LoadConfigs(*imageConfig, *serviceConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = CreateRuntimeConfig(*imageConfig, *serviceConfig, runtimeDir, *runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    return ErrorEnum::eNone;
}

Error Instance::Stop()
{
    Error stopErr;

    auto runtimeDir = fs::path(mConfig.mRuntimeDir) / mInstanceID;

    if (auto err = mFileSystem.RemoveAll(runtimeDir.string()); !err.IsNone()) {
        stopErr = AOS_ERROR_WRAP(err);
    }

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

Error Instance::LoadConfigs(oci::ImageConfig& imageConfig, oci::ServiceConfig& serviceConfig)
{
    auto path = std::make_unique<StaticString<cFilePathLen>>();

    if (auto err = mItemInfoProvider.GetBlobPath(mInstanceInfo.mManifestDigest, *path); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    auto manifest = std::make_unique<oci::ImageManifest>();

    if (auto err = mOCISpec.LoadImageManifest(*path, *manifest); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = mItemInfoProvider.GetBlobPath(manifest->mConfig.mDigest, *path); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = mOCISpec.LoadImageConfig(*path, imageConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (!manifest->mAosService.HasValue()) {
        return AOS_ERROR_WRAP(Error(ErrorEnum::eNotFound, "service config not found in manifest"));
    }

    if (auto err = mItemInfoProvider.GetBlobPath(manifest->mAosService->mDigest, *path); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = mOCISpec.LoadServiceConfig(*path, serviceConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    return ErrorEnum::eNone;
}

Error Instance::CreateRuntimeConfig(const oci::ImageConfig& imageConfig, const oci::ServiceConfig& serviceConfig,
    const std::string& runtimeDir, oci::RuntimeConfig& runtimeConfig)
{
    LOG_DBG() << "Create runtime config" << Log::Field("instanceID", mInstanceID.c_str());

    if (auto err = oci::CreateExampleRuntimeConfig(runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    runtimeConfig.mProcess->mTerminal  = false;
    runtimeConfig.mProcess->mUser.mUID = mInstanceInfo.mUID;
    runtimeConfig.mProcess->mUser.mGID = mInstanceInfo.mGID;

    if (auto err = runtimeConfig.mLinux->mCgroupsPath.Assign(JoinPath(cCgroupsPath, mInstanceID).c_str());
        !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = runtimeConfig.mRoot->mPath.Assign(JoinPath(runtimeDir, cRootFSDir).c_str()); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    runtimeConfig.mRoot->mReadonly = false;

    if (auto err = BindHostDirs(runtimeConfig); !err.IsNone()) {
        return err;
    }

    auto [instanceNetns, err] = mNetworkManager.GetNetnsPath(mInstanceID.c_str());
    if (!err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (err = AddNamespace(oci::LinuxNamespace {oci::LinuxNamespaceEnum::eNetwork, instanceNetns}, runtimeConfig);
        !err.IsNone()) {
        return err;
    }

    if (err = CreateAosEnvVars(runtimeConfig); !err.IsNone()) {
        return err;
    }

    if (err = ApplyImageConfig(imageConfig, runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (err = ApplyServiceConfig(serviceConfig, runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (err = ApplyStateStorage(runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (err = OverrideEnvVars(runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (err = mOCISpec.SaveRuntimeConfig(JoinPath(runtimeDir, cRuntimeSpecFile).c_str(), runtimeConfig);
        !err.IsNone()) {
        return err;
    }

    return ErrorEnum::eNone;
}

Error Instance::BindHostDirs(oci::RuntimeConfig& runtimeConfig)
{
    for (const auto& hostEntry : cBindEtcEntries) {
        auto path  = JoinPath("/etc", hostEntry);
        auto mount = std::make_unique<Mount>(path, path, "bind", "bind,ro");

        if (auto err = AddMount(*mount, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    return ErrorEnum::eNone;
}

Error Instance::CreateAosEnvVars(oci::RuntimeConfig& runtimeConfig)
{
    auto                     envVars = std::make_unique<StaticArray<StaticString<cEnvVarLen>, cMaxNumEnvVariables>>();
    StaticString<cEnvVarLen> envVar;

    if (auto err = envVar.Format("%s=%s", cEnvAosItemID, mInstanceInfo.mItemID.CStr()); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVars->PushBack(envVar); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVar.Format("%s=%s", cEnvAosSubjectID, mInstanceInfo.mSubjectID.CStr()); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVars->PushBack(envVar); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVar.Format("%s=%d", cEnvAosInstanceIndex, mInstanceInfo.mInstance); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVars->PushBack(envVar); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVar.Format("%s=%s", cEnvAosInstanceID, mInstanceID.c_str()); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = envVars->PushBack(envVar); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    if (auto err = AddEnvVars(*envVars, runtimeConfig); !err.IsNone()) {
        return err;
    }

    return ErrorEnum::eNone;
}

Error Instance::OverrideEnvVars(oci::RuntimeConfig& runtimeConfig)
{
    auto                     envVars = std::make_unique<StaticArray<StaticString<cEnvVarLen>, cMaxNumEnvVariables>>();
    StaticString<cEnvVarLen> envVar;

    for (const auto& overrideEnvVar : mInstanceInfo.mEnvVars) {
        if (auto err = envVar.Format("%s=%s", overrideEnvVar.mName.CStr(), overrideEnvVar.mValue.CStr());
            !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        if (auto err = envVars->PushBack(envVar); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }
    }

    if (auto err = AddEnvVars(*envVars, runtimeConfig); !err.IsNone()) {
        return err;
    }

    return ErrorEnum::eNone;
}

Error Instance::ApplyImageConfig(const oci::ImageConfig& imageConfig, oci::RuntimeConfig& runtimeConfig)
{
    runtimeConfig.mProcess->mArgs.Clear();

    for (const auto& arg : imageConfig.mConfig.mEntryPoint) {
        if (auto err = runtimeConfig.mProcess->mArgs.PushBack(arg); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }
    }

    for (const auto& arg : imageConfig.mConfig.mCmd) {
        if (auto err = runtimeConfig.mProcess->mArgs.PushBack(arg); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }
    }

    runtimeConfig.mProcess->mCwd = imageConfig.mConfig.mWorkingDir;

    if (runtimeConfig.mProcess->mCwd.IsEmpty()) {
        runtimeConfig.mProcess->mCwd = "/";
    }

    if (auto err = AddEnvVars(imageConfig.mConfig.mEnv, runtimeConfig); !err.IsNone()) {
        return AOS_ERROR_WRAP(err);
    }

    return ErrorEnum::eNone;
}

Error Instance::ApplyServiceConfig(const oci::ServiceConfig& serviceConfig, oci::RuntimeConfig& runtimeConfig)
{
    if (serviceConfig.mHostname.HasValue()) {
        runtimeConfig.mHostname = *serviceConfig.mHostname;
    }

    runtimeConfig.mLinux->mSysctl = serviceConfig.mSysctl;

    if (serviceConfig.mQuotas.mCPUDMIPSLimit.HasValue()) {
        int64_t quota
            = *serviceConfig.mQuotas.mCPUDMIPSLimit * cDefaultCPUPeriod * GetNumCPUCores() / mNodeInfo.mMaxDMIPS;
        if (quota < cMinCPUQuota) {
            quota = cMinCPUQuota;
        }

        if (auto err = SetCPULimit(quota, cDefaultCPUPeriod, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (serviceConfig.mQuotas.mRAMLimit.HasValue()) {
        if (auto err = SetRAMLimit(*serviceConfig.mQuotas.mRAMLimit, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (serviceConfig.mQuotas.mPIDsLimit.HasValue()) {
        auto pidLimit = *serviceConfig.mQuotas.mPIDsLimit;

        if (auto err = SetPIDLimit(pidLimit, runtimeConfig); !err.IsNone()) {
            return err;
        }

        if (auto err = AddRLimit(oci::POSIXRlimit {"RLIMIT_NPROC", pidLimit, pidLimit}, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (serviceConfig.mQuotas.mNoFileLimit.HasValue()) {
        auto noFileLimit = *serviceConfig.mQuotas.mNoFileLimit;

        if (auto err = AddRLimit(oci::POSIXRlimit {"RLIMIT_NOFILE", noFileLimit, noFileLimit}, runtimeConfig);
            !err.IsNone()) {
            return err;
        }
    }

    if (serviceConfig.mQuotas.mTmpLimit.HasValue()) {
        StaticString<cFSMountOptionLen> tmpFSOpts;

        if (auto err = tmpFSOpts.Format("nosuid,strictatime,mode=1777,size=%lu", *serviceConfig.mQuotas.mTmpLimit);
            !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        auto mount = std::make_unique<Mount>("tmpfs", "/tmp", "tmpfs", tmpFSOpts);

        if (auto err = AddMount(*mount, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (!serviceConfig.mPermissions.IsEmpty()) {
        auto [secret, err] = mPermHandler.RegisterInstance(
            static_cast<const InstanceIdent&>(mInstanceInfo), serviceConfig.mPermissions);
        if (!err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        mPermissionsRegistered = true;

        StaticString<cEnvVarLen> envVar;

        if (err = envVar.Format("%s=%s", cEnvAosSecret, secret.CStr()); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        if (err = AddEnvVars(Array<StaticString<cEnvVarLen>>(&envVar, 1), runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (auto err = SetResources(serviceConfig.mResources, runtimeConfig); !err.IsNone()) {
        return err;
    }

    return ErrorEnum::eNone;
}

Error Instance::ApplyStateStorage(oci::RuntimeConfig& runtimeConfig)
{
    if (!mInstanceInfo.mStatePath.IsEmpty()) {
        auto [absPath, err] = mFileSystem.GetAbsPath(GetFullStatePath(mInstanceInfo.mStatePath));
        if (!err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        auto mount = MakeShared<Mount>(&sAllocator, absPath, cInstanceStateFile, "bind", "bind,rw");

        if (err = AddMount(*mount, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    if (!mInstanceInfo.mStoragePath.IsEmpty()) {
        auto [absPath, err] = mRuntime.GetAbsPath(GetFullStoragePath(mInstanceInfo.mStoragePath));
        if (!err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        auto mount = MakeShared<Mount>(&sAllocator, absPath, cInstanceStorageDir, "bind", "bind,rw");

        if (err = AddMount(*mount, runtimeConfig); !err.IsNone()) {
            return err;
        }
    }

    return ErrorEnum::eNone;
}

size_t Instance::GetNumCPUCores() const
{
    int numCores = std::accumulate(mNodeInfo.mCPUs.begin(), mNodeInfo.mCPUs.end(), 0,
        [](int sum, const auto& cpu) { return sum + cpu.mNumCores; });

    if (numCores == 0) {
        LOG_WRN() << "Can't identify number of CPU cores, default value (1) will be taken"
                  << Log::Field("instanceID", mInstanceID.c_str());

        numCores = 1;
    }

    return numCores;
}

Error Instance::SetResources(const Array<StaticString<cResourceNameLen>>& resources, oci::RuntimeSpec& runtimeSpec)
{
    for (const auto& resource : resources) {
        auto resourceInfo = MakeUnique<ResourceInfo>(&sAllocator);

        if (auto err = mResourceManager.GetResourceInfo(resource, *resourceInfo); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        for (const auto& group : resourceInfo->mGroups) {
            auto [gid, err] = mRuntime.GetGIDByName(group);
            if (!err.IsNone()) {
                return AOS_ERROR_WRAP(err);
            }

            if (err = AddAdditionalGID(gid, runtimeSpec); !err.IsNone()) {
                return err;
            }
        }

        for (const auto& mount : resourceInfo->mMounts) {
            if (auto err = AddMount(mount, runtimeSpec); !err.IsNone()) {
                return err;
            }
        }

        if (auto err = AddEnvVars(resourceInfo->mEnv, runtimeSpec); !err.IsNone()) {
            return err;
        }
    }

    return ErrorEnum::eNone;
}

Error Instance::SetDevices(const Array<oci::ServiceDevice>& devices, oci::RuntimeSpec& runtimeSpec)
{
    for (const auto& device : devices) {
        auto deviceInfo = MakeUnique<DeviceInfo>(&sAllocator);

        LOG_DBG() << "Set device" << Log::Field("instanceID", mInstanceID) << Log::Field("device", device.mDevice);

        if (auto err = mResourceManager.GetDeviceInfo(device.mDevice, *deviceInfo); !err.IsNone()) {
            return AOS_ERROR_WRAP(err);
        }

        auto ociDevices = MakeUnique<StaticArray<oci::LinuxDevice, cMaxNumHostDevices>>(&sAllocator);

        for (const auto& hostDevice : deviceInfo->mHostDevices) {
            StaticArray<StaticString<cDeviceNameLen>, 2> devicePaths;

            if (auto err = hostDevice.Split(devicePaths, ':'); !err.IsNone()) {
                return AOS_ERROR_WRAP(err);
            }

            LOG_DBG() << "Populate host device" << Log::Field("instanceID", mInstanceID)
                      << Log::Field("hostDevice", devicePaths[0]);

            if (auto err = mRuntime.PopulateHostDevices(devicePaths[0], *ociDevices); !err.IsNone()) {
                return AOS_ERROR_WRAP(err);
            }

            if (devicePaths.Size() == 2) {
                LOG_DBG() << "Map host device" << Log::Field("instanceID", mInstanceID)
                          << Log::Field("hostDevice", devicePaths[0]) << Log::Field("containerDevice", devicePaths[1]);

                for (auto& ociDevice : *ociDevices) {
                    if (auto err = ociDevice.mPath.Replace(devicePaths[0], devicePaths[1], 1); !err.IsNone()) {
                        return AOS_ERROR_WRAP(err);
                    }
                }
            }
        }

        for (const auto& ociDevice : *ociDevices) {
            if (auto err = AddDevice(ociDevice, device.mPermissions, runtimeSpec); !err.IsNone()) {
                return err;
            }
        }

        for (const auto& group : deviceInfo->mGroups) {
            auto [gid, err] = mRuntime.GetGIDByName(group);
            if (!err.IsNone()) {
                return AOS_ERROR_WRAP(err);
            }

            if (err = AddAdditionalGID(gid, runtimeSpec); !err.IsNone()) {
                return err;
            }
        }
    }

    return ErrorEnum::eNone;
}

} // namespace aos::sm::launcher
