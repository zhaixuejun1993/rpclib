#pragma once

#ifndef PIMPL_H_RPC
#define PIMPL_H_RPC

//! \brief Declares a pimpl pointer.
#define RPCLIB_DECLARE_PIMPL()                                                \
    struct impl; std::unique_ptr<impl> pimpl;

#endif /* end of include guard: PIMPL_H_RPC */
