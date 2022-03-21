// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include "rpc/ITTProfiler.h"

#ifdef ENABLE_ITT

static __itt_domain *itt_domain = nullptr;
namespace ITTPerfUtils
{
    ITTProfilingTask::ITTProfilingTask(const char *name) {
        taskBegin(name);
    }

    ITTProfilingTask::ITTProfilingTask(const std::string &name) {
        taskBegin(name.c_str());
    }

    ITTProfilingTask::~ITTProfilingTask() {
        taskEnd();
    }

    void ITTProfilingTask::taskBegin(const char *name) {
        if (itt_domain == nullptr) {
            itt_domain = __itt_domain_create("ITTPerfUtils");
        }
        if (itt_domain)
            __itt_task_begin(itt_domain, __itt_null, __itt_null, __itt_string_handle_create(name));
    }

    void ITTProfilingTask::taskEnd() {
        if (itt_domain)
            __itt_task_end(itt_domain);
    }
}

#endif // ENABLE_ITT