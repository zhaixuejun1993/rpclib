#pragma once

#ifndef NOT_H_RPC
#define NOT_H_RPC

#include "rpc/detail/bool.h"

namespace rpc {
namespace detail {

template<typename B>
using not_ = bool_<!B::value>;

}
}

#endif /* end of include guard: NOT_H_RPC */
