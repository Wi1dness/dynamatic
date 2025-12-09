//===- HandshakeInsertCoverPoint.h - Insert coverpoint ops ------*- C++ -*-===//
//
// This file declares the --handshake-insert-coverpoint pass.
//
//===----------------------------------------------------------------------===//

#ifndef DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTCOVERPOINT_H
#define DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTCOVERPOINT_H

#include "dynamatic/Support/DynamaticPass.h"

namespace dynamatic {

#define GEN_PASS_DECL_HANDSHAKEINSERTCOVERPOINT
#define GEN_PASS_DEF_HANDSHAKEINSERTCOVERPOINT
#include "dynamatic/Transforms/Passes.h.inc"

std::unique_ptr<dynamatic::DynamaticPass> createHandshakeInsertCoverPoint();

} // namespace dynamatic

#endif // DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTCOVERPOINT_H
