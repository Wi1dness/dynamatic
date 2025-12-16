//===- HandshakeInsertSchedCP.h - Insert schedule probes -------*- C++ -*-===//
//
// This file declares the --handshake-insert-sched-cp pass.
//
//===----------------------------------------------------------------------===//

#ifndef DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTSCHEDCP_H
#define DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTSCHEDCP_H

#include "dynamatic/Support/DynamaticPass.h"

namespace dynamatic {

#define GEN_PASS_DECL_HANDSHAKEINSERTSCHEDCP
#define GEN_PASS_DEF_HANDSHAKEINSERTSCHEDCP
#include "dynamatic/Transforms/Passes.h.inc"

std::unique_ptr<dynamatic::DynamaticPass> createHandshakeInsertSchedCP();

} // namespace dynamatic

#endif // DYNAMATIC_TRANSFORMS_HANDSHAKEINSERTSCHEDCP_H
