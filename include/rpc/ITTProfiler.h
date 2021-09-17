//
//Copyright (C) 2019 Intel Corporation
//
//SPDX-License-Identifier: MIT
//

// Implement for profile executable file with ITT(Instrumentation and Tracing Technology API) using Intel VTune
// Please refer to https://software.intel.com/content/www/us/en/develop/documentation/vtune-help/top/api-support/instrumentation-and-tracing-technology-apis
// If you want use vtune attach our program, you should export INTEL_LIBITTNOTIFY64=/opt/intel/vtune_profiler/lib64/runtime/libittnotify_collector.so, please refer to 
//	https://software.intel.com/content/www/us/en/develop/documentation/vtune-help/top/api-support/instrumentation-and-tracing-technology-apis/basic-usage-and-configuration/attaching-itt-apis-to-a-launched-application.html

#ifndef ITTPERFUTILS_ITT_H
#define ITTPERFUTILS_ITT_H

#include <string>
#include <ittnotify.h>
#include "config.h"

namespace ITTPerfUtils {
#if defined(ENABLE_ITT)
#define ITT_PROFILING_TASK(NAME) ITTPerfUtils::ITTProfilingTask profileTask(NAME)

    class ITTProfilingTask {
    public:
        ITTProfilingTask(const char *name);

        ITTProfilingTask(const std::string &name);

        ~ITTProfilingTask();

    private:
        void taskBegin(const char *name);

        void taskEnd();
    };

#else

#define ITT_PROFILING_TASK(NAME) {}

#endif
}

#endif //ITTPERFUTILS_ITT_H
