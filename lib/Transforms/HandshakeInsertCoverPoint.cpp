//===- HandshakeInsertCoverPoint.cpp - Insert coverpoint ops -----*- C++ -*-===//
//
// Inserts transparent `handshake.coverpoint` operations in front of selected
// data-path users so that downstream tooling can observe ready/valid activity.
//
//===----------------------------------------------------------------------===//

#include "dynamatic/Transforms/HandshakeInsertCoverPoint.h"
#include "dynamatic/Dialect/Handshake/HandshakeOps.h"
#include "dynamatic/Dialect/Handshake/HandshakeTypes.h"
#include "dynamatic/Support/CFG.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include <cstdint>

using namespace dynamatic;

namespace {

class HandshakeInsertCoverPointPass
    : public dynamatic::impl::HandshakeInsertCoverPointBase<
          HandshakeInsertCoverPointPass> {

public:
  using dynamatic::impl::HandshakeInsertCoverPointBase<
      HandshakeInsertCoverPointPass>::HandshakeInsertCoverPointBase;

  void runDynamaticPass() override {
    mlir::ModuleOp mod = getOperation();
    uint32_t nextCovId = 0;

    mod.walk([&](handshake::FuncOp func) {
      if (func.isExternal())
        return;
      doInstrument(func, nextCovId);
    });
  }

private:
  static bool skipOp(mlir::Operation *op) {
    return mlir::isa<handshake::CoverPointOp, handshake::BranchOp,
                     handshake::ForkOp, handshake::LazyForkOp,
                     handshake::BufferOp, handshake::SinkOp>(op);
  }

  static bool skipOperand(mlir::OpOperand &operand) {
    mlir::Value input = operand.get();
    if (!mlir::isa<handshake::ChannelType>(input.getType()))
      return true;
    mlir::Operation *def = input.getDefiningOp();
    if (!def)
      return true;
    mlir::Operation *user = operand.getOwner();
    unsigned operandIdx = operand.getOperandNumber();
    if (mlir::isa<handshake::MuxOp>(user) && operandIdx == 0)
      return true;
    if (auto cbranch = mlir::dyn_cast<handshake::ConditionalBranchOp>(user);
        cbranch && operandIdx != 0)
      return true;
    return mlir::isa<handshake::CoverPointOp, handshake::ControlMergeOp,
                     handshake::MergeOp, handshake::MuxOp,
                     handshake::JoinOp>(def);
  }

  static void doInstrument(handshake::FuncOp func,
                                 uint32_t &nextCovId) {
    for (mlir::Block &block : func.getBody()) {
      for (mlir::Operation &operation : llvm::make_early_inc_range(block)) {
        if (skipOp(&operation))
          continue;

        llvm::SmallVector<mlir::OpOperand *> operandsToInstrument;
        operandsToInstrument.reserve(operation.getNumOperands());
        for (mlir::OpOperand &operand : operation.getOpOperands()) {
          if (skipOperand(operand))
            continue;
          operandsToInstrument.push_back(&operand);
        }

        if (operandsToInstrument.empty())
          continue;

        mlir::OpBuilder builder(&operation);
        builder.setInsertionPoint(&operation);
        for (mlir::OpOperand *operand : operandsToInstrument) {
          mlir::Value input = operand->get();
          auto coverpoint = builder.create<handshake::CoverPointOp>(
              operation.getLoc(), input.getType(), input);
          auto covIdType =
              builder.getIntegerType(32, mlir::IntegerType::Unsigned);
          auto covIdAttr =
              builder.getIntegerAttr(covIdType, nextCovId++);
          coverpoint->setAttr(handshake::CoverPointOp::COV_ID_ATTR_NAME,
                              covIdAttr);
          // Prefer attaching the same logical block as the user to preserve
          // downstream metadata; fall back to the source value when needed.
          if (!inheritBB(&operation, coverpoint))
            inheritBBFromValue(input, coverpoint);
          operand->set(coverpoint.getResult());
        }
      }
    }
  }
};

} // namespace

std::unique_ptr<dynamatic::DynamaticPass>
dynamatic::createHandshakeInsertCoverPoint() {
  return std::make_unique<HandshakeInsertCoverPointPass>();
}
