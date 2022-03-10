#pragma once

#ifndef CONFIG_H_L7IVDSPZ
#define CONFIG_H_L7IVDSPZ

#include <cstddef>
#include <cstdint>

#include "rpc/compatibility.h"

#ifndef _WIN32
#define IPC_PREFIX "/tmp/ov_service_rpc_"
#else
#define IPC_PREFIX  "\\\\.\\pipe\\ov_service_rpc_"
#endif

#define ENABLE_ITT

namespace rpc
{

using session_id_t = std::intptr_t;

//! \brief Constants used in the library
struct constants RPCLIB_FINAL {
    static RPCLIB_CONSTEXPR std::size_t DEFAULT_BUFFER_SIZE = 1024 << 10;
};

} /* rpc */

// This define allows the end user to replace the msgpack dependency.
// To do so, one has to delete the msgpack headers that are
// in the rpclib directory. The other messagepack headers don't
// need to be stored in place of the others. Finally, the RPCLIB_MSGPACK
// macro has to be changed to the namespace name that this new
// msgpack uses (usually "msgpack", unless it is changed manually)
#ifndef RPCLIB_MSGPACK
#define RPCLIB_MSGPACK clmdep_msgpack
#endif /* ifndef RPCLIB_MSGPACK */

#ifndef RPCLIB_CXX_STANDARD
#define RPCLIB_CXX_STANDARD 11
#endif

#endif /* end of include guard: CONFIG_H_L7IVDSPZ */
