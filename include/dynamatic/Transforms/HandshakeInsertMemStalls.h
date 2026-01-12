//===- HandshakeInsertMemStalls.h - Insert handshake.stall ops ---*- C++ -*-===//
//
// This file declares the --handshake-insert-mem-stalls pass.
//
//===----------------------------------------------------------------------===//

#ifndef DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTMEMSTALLS_H
#define DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTMEMSTALLS_H

#include "dynamatic/Support/DynamaticPass.h"

namespace dynamatic {

#define GEN_PASS_DECL_HANDSHAKEINSERTMEMSTALLS
#define GEN_PASS_DEF_HANDSHAKEINSERTMEMSTALLS
#include "dynamatic/Transforms/Passes.h.inc"

std::unique_ptr<dynamatic::DynamaticPass> createHandshakeInsertMemStalls();

} // namespace dynamatic

#endif // DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTMEMSTALLS_H
