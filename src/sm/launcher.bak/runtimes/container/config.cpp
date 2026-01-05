/*
 * Copyright (C) 2025 EPAM Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.hpp"

namespace aos::sm::launcher {

void ParseContainerConfig(const common::utils::CaseInsensitiveObjectWrapper& object, ContainerConfig& config)
{
    config.mRuntimeDir = object.GetValue<std::string>("runtimeDir", "/run/aos/runtime");
}

} // namespace aos::sm::launcher
