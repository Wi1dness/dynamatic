//===- HandshakeInsertSchedCP.cpp - Insert sched_cp ops --------*- C++ -*-===//
//
// Inserts transparent `handshake.sched_cp` operations on control channels so
// that downstream tooling can observe dynamic scheduling behaviour.
//
//===----------------------------------------------------------------------===//

#include "dynamatic/Transforms/HandshakeInsertSchedCP.h"

#include "dynamatic/Dialect/Handshake/HandshakeOps.h"
#include "dynamatic/Dialect/Handshake/HandshakeTypes.h"
#include "dynamatic/Support/CFG.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Matchers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <optional>
#include <cstdlib>
#include <string_view>

using namespace dynamatic;

namespace {

class HandshakeInsertSchedCPPass
    : public dynamatic::impl::HandshakeInsertSchedCPBase<
          HandshakeInsertSchedCPPass> {
public:
  using dynamatic::impl::HandshakeInsertSchedCPBase<
      HandshakeInsertSchedCPPass>::HandshakeInsertSchedCPBase;

  void runDynamaticPass() override {
    mlir::ModuleOp mod = getOperation();
    uint32_t nextSchedId = 0;

    mod.walk([&](handshake::FuncOp func) {
      if (func.isExternal())
        return;
      addSideband(func);
      rerouteControlEnd(func);
      doInsert(func, nextSchedId);
    });
  }

private:
  static constexpr llvm::StringLiteral TIMESTAMP_SIGNAL_NAME =
      "schedcp_ts";
  static constexpr unsigned TIMESTAMP_WIDTH = 64;
  static constexpr llvm::StringLiteral COVSUM_SIGNAL_NAME =
      "schedcp_covsum";
  static constexpr unsigned COVSUM_WIDTH = 32;
  static constexpr llvm::StringLiteral COMPLETION_JOIN_ATTR_NAME =
    "completion_join";
  static constexpr llvm::StringLiteral EXIT_PRED_BB_ATTR_NAME =
    "dynamatic.exit_pred_bb";

  /// Propagates the timestamp/covsum extra signals across the function's control network
  static void addSideband(handshake::FuncOp func) {
    MLIRContext *ctx = func.getContext();
    handshake::ExtraSignal timestampSignal(
        TIMESTAMP_SIGNAL_NAME,
        IntegerType::get(ctx, TIMESTAMP_WIDTH), /*downstream=*/true);
    handshake::ExtraSignal covsumSignal(
        COVSUM_SIGNAL_NAME,
        IntegerType::get(ctx, COVSUM_WIDTH), /*downstream=*/true);

    Block &entryBlock = getEntryBlock(func);
    std::optional<unsigned> startArgIndex =
        getNamedIndex(func.getArgNames(), "start");

    auto addSignal = [&](Value value) {
      auto ctrlType = value.getType().dyn_cast<handshake::ControlType>();
      if (!ctrlType)
        return;

      bool hasTimestamp = ctrlType.hasExtraSignal(TIMESTAMP_SIGNAL_NAME);
      bool hasCovsum = ctrlType.hasExtraSignal(COVSUM_SIGNAL_NAME);
      if (hasTimestamp && hasCovsum)
        return;

      SmallVector<handshake::ExtraSignal> extras(
          ctrlType.getExtraSignals().begin(),
          ctrlType.getExtraSignals().end());
      if (!hasTimestamp)
        extras.push_back(timestampSignal);
      if (!hasCovsum)
        extras.push_back(covsumSignal);
      value.setType(ctrlType.copyWithExtraSignals(extras));
    };

    auto shouldAnnotate = [&](Value value) -> bool {
      auto ctrlType = value.getType().dyn_cast<handshake::ControlType>();
      if (!ctrlType)
        return false;

      if (auto blockArg = value.dyn_cast<BlockArgument>())
        return startArgIndex && blockArg.getOwner() == &entryBlock &&
               blockArg.getArgNumber() == *startArgIndex;

      Operation *def = value.getDefiningOp();
      if (!def)
        return false;

      if (isa<handshake::MemoryControllerOp, handshake::LSQOp,
        handshake::BundleOp, handshake::UnbundleOp,
        handshake::SinkOp, handshake::SourceOp,
        handshake::StripExtraSignalOp>(def))
        return false;

      return true;
    };

    auto stripForUse = [&](Value producer, OpOperand &use) {
      auto ctrlType = producer.getType().dyn_cast<handshake::ControlType>();
      if (!ctrlType || (!ctrlType.hasExtraSignal(TIMESTAMP_SIGNAL_NAME) &&
                        !ctrlType.hasExtraSignal(COVSUM_SIGNAL_NAME)))
        return;

      Operation *user = use.getOwner();
      if (!isa<handshake::MemoryControllerOp, handshake::LSQOp,
        handshake::ConstantOp, handshake::SinkOp>(user))
        return;

      mlir::OpBuilder builder(user);
      builder.setInsertionPoint(user);
      auto strip = builder.create<handshake::StripExtraSignalOp>(
          user->getLoc(), producer);
      if (Operation *producerOp = producer.getDefiningOp()) {
        if (!dynamatic::inheritBB(producerOp, strip))
          dynamatic::inheritBBFromValue(producer, strip);
      } else {
        dynamatic::inheritBBFromValue(producer, strip);
      }
      use.set(strip.getResult());
    };

    auto endOp = getEndOp(func);
    std::optional<unsigned> endResIndex =
        getNamedIndex(func.getResNames(), "end");

    SmallVector<Value, 32> worklist;
    llvm::DenseSet<Value> visited;

    auto enqueueControl = [&](Value v) {
      if (!v || !v.getType().isa<handshake::ControlType>())
        return;
      if (Operation *def = v.getDefiningOp())
        if (isa<handshake::StripExtraSignalOp>(def))
          return;
      worklist.push_back(v);
    };

    if (startArgIndex)
      enqueueControl(entryBlock.getArgument(*startArgIndex));

    while (!worklist.empty()) {
      Value current = worklist.pop_back_val();
      if (!current)
        continue;
      if (!visited.insert(current).second)
        continue;

      if (shouldAnnotate(current))
        addSignal(current);

      if (Operation *def = current.getDefiningOp()) {
        for (Value result : def->getResults())
          enqueueControl(result);
      }

      for (OpOperand &use : current.getUses()) {
        Operation *user = use.getOwner();
        stripForUse(current, use);
        for (Value operand : user->getOperands())
          if (operand != current)
            enqueueControl(operand);
        for (Value result : user->getResults())
          enqueueControl(result);
      }
    }

    auto fnType = func.getFunctionType();
    SmallVector<Type> argTypes(fnType.getInputs().begin(), fnType.getInputs().end());
    for (const auto &[idx, arg] : llvm::enumerate(entryBlock.getArguments()))
      if (idx < argTypes.size())
        argTypes[idx] = arg.getType();

    SmallVector<Type> resultTypes(fnType.getResults().begin(),
                                  fnType.getResults().end());
    if (endOp && endResIndex)
      addSignal(endOp.getOperand(*endResIndex));
    if (endOp)
      for (const auto &[idx, operand] : llvm::enumerate(endOp->getOperands()))
        if (idx < resultTypes.size())
          resultTypes[idx] = operand.getType();

    auto newType = FunctionType::get(func.getContext(), argTypes, resultTypes);
    func.setFunctionType(newType);
  }

  /// Returns the index of `target` in the provided name attribute array.
  static std::optional<unsigned> getNamedIndex(ArrayAttr namesAttr,
                                               StringRef target) {
    if (!namesAttr)
      return std::nullopt;
    for (const auto &[idx, attr] : llvm::enumerate(namesAttr)) {
      if (auto strAttr = dyn_cast<StringAttr>(attr);
          strAttr && strAttr.getValue() == target)
        return idx;
    }
    return std::nullopt;
  }

  /// Fetches the unique entry block of the handshake function.
  static Block &getEntryBlock(handshake::FuncOp func) {
    return func.getBody().front();
  }

  /// Convenience accessor for the function's terminating `handshake.end`.
  static handshake::EndOp getEndOp(handshake::FuncOp func) {
    return dyn_cast<handshake::EndOp>(getEntryBlock(func).getTerminator());
  }

  /// Returns the SSA value corresponding to the `start` argument, if any.
  // static Value getStartSignal(handshake::FuncOp func) {
  //   std::optional<unsigned> startIdx =
  //       getNamedIndex(func.getArgNames(), "start");
  //   if (!startIdx)
  //     return nullptr;
  //   Block &entryBlock = getEntryBlock(func);
  //   if (*startIdx >= entryBlock.getNumArguments())
  //     return nullptr;
  //   return entryBlock.getArgument(*startIdx);
  // }

  /// Checks whether every user of `val` is a pure buffer (allowing us to skip
  /// intermediate buffer chains when seeking meaningful control tokens).
  static bool usersAreOnlyBuffers(Value val) {
    if (val.use_empty())
      return false;
    for (Operation *user : val.getUsers())
      if (!isa<handshake::BufferOp>(user))
        return false;
    return true;
  }

  /// Mirrors `HandshakeCFG::getControlValues` to collect one representative
  /// control SSA value per logic basic block. Unlike the CFG utility, we also
  /// register non-control-merge producers (skipping buffer-only chains) so the
  /// pass can still recover a token when optimization deleted the block's
  /// `handshake.control_merge`. Fails if conflicting tokens are discovered for
  /// the same block.
  static LogicalResult collectBlockControls(handshake::FuncOp func,
                                            llvm::DenseMap<unsigned, Value> &ctrlVals) {
    llvm::DenseSet<Operation *> exploredOps;
    llvm::SetVector<Operation *> ctrlOps;

    auto addToCtrlOps = [&](auto users) {
      for (Operation *userOp : users) {
        if (!exploredOps.contains(userOp))
          ctrlOps.insert(userOp);
      }
    };

    auto updateCtrl = [&](unsigned bb, Value newCtrl) -> LogicalResult {
      if (!newCtrl)
        return success();
      auto [it, inserted] = ctrlVals.insert({bb, newCtrl});
      if (!inserted && it->second != newCtrl)
        return failure();
      return success();
    };

    Value entryCtrl = func.getArguments().back();
    if (failed(updateCtrl(ENTRY_BB, entryCtrl)))
      return failure();
    addToCtrlOps(entryCtrl.getUsers());

    auto recordCtrlFromOp = [&](unsigned bb, Operation *op) -> LogicalResult {
      if (ctrlVals.find(bb) != ctrlVals.end())
        return success();
      for (Value result : op->getResults()) {
        if (!result.getType().isa<handshake::ControlType>())
          continue;
        if (isa<handshake::BufferOp>(op) && usersAreOnlyBuffers(result))
          continue;
        return updateCtrl(bb, result);
      }
      return success();
    };

    while (!ctrlOps.empty()) {
      Operation *ctrlOp = ctrlOps.pop_back_val();
      exploredOps.insert(ctrlOp);

      if (cannotBelongToCFG(ctrlOp))
        continue;

      std::optional<unsigned> bbOpt = getLogicBB(ctrlOp);
      // Some Handshake ops may not carry CFG metadata (e.g., after certain
      // canonicalizations/materializations). In that case, we can't reliably
      // associate them with a logic basic block, so skip them instead of
      // crashing by dereferencing a disengaged optional.
      if (!bbOpt)
        continue;
      unsigned bb = *bbOpt;

      LogicalResult res = llvm::TypeSwitch<Operation *, LogicalResult>(ctrlOp)
                              .Case<handshake::ForkOp, handshake::LazyForkOp,
                                    handshake::BufferOp, handshake::BranchOp,
                                    handshake::ConditionalBranchOp,
                                    handshake::MuxOp, handshake::MergeOp>([&](auto) {
                                if (failed(recordCtrlFromOp(bb, ctrlOp)))
                                  return failure();
                                addToCtrlOps(ctrlOp->getUsers());
                                return success();
                              })
                              .Case<handshake::ControlMergeOp>([&](auto) {
                                OpResult mergeRes = ctrlOp->getResult(0);
                                addToCtrlOps(mergeRes.getUsers());
                                return updateCtrl(bb, mergeRes);
                              })
                              .Default([&](auto) { return success(); });
      if (failed(res))
        return failure();
    }

    return success();
  }

  /// Returns the control SSA value that represents the block identified by
  /// `bbID`, emitting an error if discovery fails.
  static Value getBlockControl(handshake::FuncOp func, unsigned bbID) {
    llvm::DenseMap<unsigned, Value> ctrlVals;
    if (succeeded(collectBlockControls(func, ctrlVals))) {
      if (auto it = ctrlVals.find(bbID); it != ctrlVals.end())
        return it->second;
    }

    func.emitOpError()
        << "failed to find control token for logic BB #" << bbID;
    return nullptr;
  }

  /// Attempts to recover a completion control value produced in `predBB` that
  /// is currently only consumed by sink operations. This supports cases where
  /// the exit path got optimized into a sink that carries no BB metadata.
  ///
  /// Requires that exactly one such control value exists (after skipping
  /// buffer-only chains). On success, erases the sink(s) consuming that value
  /// and returns the recovered control value.
  static Value recoverExitCompletionTokenFromPredBB(handshake::FuncOp func,
                                                    unsigned predBB) {
    Value candidate = nullptr;

    const bool debug = []() {
      const char *v = std::getenv("DYNAMATIC_DEBUG_SCHEDCOV_COMPLETION");
      return v && v[0] != '\0' && std::string_view(v) != "0";
    }();

    auto dbg = [&](auto &&fn) {
      if (!debug)
        return;
      llvm::errs() << "[schedcov_completion] func='";
      if (auto sym = func->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()))
        llvm::errs() << sym.getValue();
      else
        llvm::errs() << "<anonymous>";
      llvm::errs() << "': ";
      fn();
      llvm::errs() << "\n";
    };

    auto dumpValueShort = [&](Value v) {
      if (!debug)
        return;
      llvm::errs() << "value=";
      if (!v) {
        llvm::errs() << "<null>";
        return;
      }
      if (auto barg = dyn_cast<BlockArgument>(v)) {
        llvm::errs() << "<block-arg #" << barg.getArgNumber() << ">";
        return;
      }
      Operation *def = v.getDefiningOp();
      if (!def) {
        llvm::errs() << "<no-def>";
        return;
      }
      llvm::errs() << def->getName();
      if (auto bbAttr = def->getAttrOfType<IntegerAttr>(BB_ATTR_NAME))
        llvm::errs() << " " << BB_ATTR_NAME << "="
                     << bbAttr.getValue().getZExtValue();
      else
        llvm::errs() << " " << BB_ATTR_NAME << "=<missing>";
    };

    // Collect the first non-buffer users reachable from v, following buffer
    // chains. If v is directly used by non-buffer ops, those are returned.
    // If v has no uses, returns an empty set.
    auto collectNonBufferUsers = [&](Value v,
                  llvm::SmallPtrSetImpl<Operation *> &out) {
      llvm::SmallVector<Value> wl;
      llvm::SmallPtrSet<Value, 16> visited;
      wl.push_back(v);
      visited.insert(v);

      while (!wl.empty()) {
        Value cur = wl.pop_back_val();
        for (Operation *user : cur.getUsers()) {
          if (auto buf = dyn_cast<handshake::BufferOp>(user)) {
            Value next = buf.getResult();
            if (visited.insert(next).second)
              wl.push_back(next);
            continue;
          }
          if (auto strip = dyn_cast<handshake::StripExtraSignalOp>(user)) {
            Value next = strip.getResult();
            if (visited.insert(next).second)
              wl.push_back(next);
            continue;
          }
          out.insert(user);
        }
      }
    };


    auto dumpTerminals = [&](StringRef label, Value v) {
      if (!debug)
        return;
      llvm::SmallPtrSet<Operation *, 16> terminals;
      collectNonBufferUsers(v, terminals);
      dbg([&] {
        llvm::errs() << label << " terminalUsers=" << terminals.size() << " (";
        dumpValueShort(v);
        llvm::errs() << ")";
      });

      unsigned idx = 0;
      for (Operation *op : terminals) {
        dbg([&] {
          llvm::errs() << "  terminal[" << idx++ << "] op='" << op->getName()
                       << "'";
          if (auto bbAttr = op->getAttrOfType<IntegerAttr>(BB_ATTR_NAME))
            llvm::errs() << " " << BB_ATTR_NAME << "="
                         << bbAttr.getValue().getZExtValue();
          else
            llvm::errs() << " " << BB_ATTR_NAME << "=<missing>";
        });
      }
    };

    // Returns true if v ultimately feeds only sinks (with optional buffer
    // chains in between).
    auto isSinkOnlyThroughBuffers = [&](Value v) -> bool {
      if (v.use_empty())
        return false;
      llvm::SmallPtrSet<Operation *, 16> terminals;
      collectNonBufferUsers(v, terminals);
      if (terminals.empty())
        return false;
      dumpTerminals("sink-check", v);
      return llvm::all_of(terminals, [](Operation *op) {
        return isa<handshake::SinkOp>(op);
      });
    };

    // Returns true if `use` ultimately feeds the ctrlEnd operand of a memory
    // interface (LSQ/MC), possibly through buffer / strip chains.
    auto isMemInterfaceCtrlEndOnlyThroughBuffers = [&](OpOperand &use) -> bool {
      // First, skip any wrapper chains that can appear between the candidate
      // value and its terminal consumer.
      auto peelWrapperUsers = [&](OpOperand &leafUse) -> llvm::SmallVector<OpOperand *> {
        llvm::SmallVector<OpOperand *> leaves;
        llvm::SmallVector<OpOperand *> wl;
        llvm::SmallPtrSet<Operation *, 16> visited;
        wl.push_back(&leafUse);

        while (!wl.empty()) {
          OpOperand *curUse = wl.pop_back_val();
          Operation *curUser = curUse->getOwner();
          if (!visited.insert(curUser).second)
            continue;

          if (auto buf = dyn_cast<handshake::BufferOp>(curUser)) {
            for (OpOperand &u : buf.getResult().getUses())
              wl.push_back(&u);
            continue;
          }
          if (auto strip = dyn_cast<handshake::StripExtraSignalOp>(curUser)) {
            for (OpOperand &u : strip.getResult().getUses())
              wl.push_back(&u);
            continue;
          }

          leaves.push_back(curUse);
        }
        return leaves;
      };

      llvm::SmallVector<OpOperand *> terminalUses = peelWrapperUsers(use);
      if (terminalUses.empty())
        return false;

      // All terminal uses must be memif ctrlEnd uses.
      for (OpOperand *tu : terminalUses) {
        Operation *tuUser = tu->getOwner();

        // MC/LSQ: rely on MemoryOpInterface::getCtrlEnd() (port-aware), which is
        // implemented for LSQ/MC in HandshakeInterfaces.cpp.
        if (auto memIf = dyn_cast<handshake::MemoryOpInterface>(tuUser)) {
          if (tu->get() != memIf.getCtrlEnd())
            return false;
          continue;
        }

        return false;
      }
      return true;
    };

    // Returns true if v is only consumed by memory-interface ctrlEnd ports
    // (LSQ ctrlEnd / MC ctrlEnd), possibly through buffer/strip chains.
    auto isMemInterfaceOnlyThroughBuffers = [&](Value v) -> bool {
      if (v.use_empty())
        return false;

      // We reuse the existing terminal dumping for debug readability, but the
      // acceptance rule is *port-aware*.
      dumpTerminals("memif-check", v);

      bool sawAnyUse = false;
      for (OpOperand &use : v.getUses()) {
        sawAnyUse = true;
        if (!isMemInterfaceCtrlEndOnlyThroughBuffers(use))
          return false;
      }
      return sawAnyUse;
    };

    auto consider = [&](Value v) -> bool {
      if (!v || !v.getType().isa<handshake::ControlType>())
        return true;

      dbg([&] {
        llvm::errs() << "consider ctrl ";
        dumpValueShort(v);
      });

      // Only accept values that are consumed exclusively by sinks (possibly via
      // buffers) or exclusively by memory-interface operations (possibly via
      // buffers). This keeps the recovery conservative.
      bool ok =
          isSinkOnlyThroughBuffers(v) || isMemInterfaceOnlyThroughBuffers(v);
      if (!ok)
        return true;

      dbg([&] {
        llvm::errs() << "  -> eligible (sink-only or memif-only)";
      });

      if (candidate && candidate != v)
        return false;
      candidate = v;
      return true;
    };

    for (Operation &op : func.getOps()) {
      if (cannotBelongToCFG(&op))
        continue;
      std::optional<unsigned> bb = getLogicBB(&op);
      if (!bb || *bb != predBB)
        continue;

      for (Value res : op.getResults()) {
        // If we're looking at a buffer result, skip pure buffer chains: we'll
        // consider the upstream non-buffer producer instead.
        if (isa<handshake::StripExtraSignalOp>(&op) || (isa<handshake::BufferOp>(&op) && usersAreOnlyBuffers(res)))
          continue;
        if (!consider(res))
          return nullptr;
      }
    }

    if (!candidate) {
      dbg([&] {
        llvm::errs() << "no eligible completion candidate found in predBB #"
                     << predBB;
      });
      return nullptr;

    }

    return candidate;
  }

  /// Reads the unique predecessor BB of the exit block from metadata if
  /// available.
  static std::optional<unsigned>
  getAnnotatedExitPredBB(handshake::EndOp endOp) {
    if (!endOp)
      return std::nullopt;
    auto attr = endOp->getAttrOfType<IntegerAttr>(EXIT_PRED_BB_ATTR_NAME);
    if (!attr)
      return std::nullopt;
    return attr.getValue().getZExtValue();
  }

  /// Rewires the `handshake.end` completion operand to use the exit-block's
  /// control token and gates it with the `start` signal via a join.
  static void rerouteControlEnd(handshake::FuncOp func) {
    auto endOp = getEndOp(func);
    if (!endOp)
      return;

    unsigned lastIdx = endOp->getNumOperands();
    if (lastIdx == 0)
      return;
    --lastIdx;

    Value originalCompletion = endOp->getOperand(lastIdx);

    // Determine the semantic exit BB from end's metadata (not from its current
    // completion operand wiring).
    unsigned exitBlockID = ENTRY_BB;
    if (auto bbAttr = endOp->getAttrOfType<IntegerAttr>(BB_ATTR_NAME))
      exitBlockID = bbAttr.getValue().getZExtValue();


  // First try to obtain a control token associated with the exit BB itself.
  // If that fails (e.g., exit BB structure got optimized away), recover a
  // sunk completion token from the explicitly annotated predecessor BB
  // (`dynamatic.exit_pred_bb`).
    Value completionCtrl = nullptr;
    llvm::DenseMap<unsigned, Value> ctrlVals;
    if (succeeded(collectBlockControls(func, ctrlVals)))
      if (auto it = ctrlVals.find(exitBlockID); it != ctrlVals.end())
        completionCtrl = it->second;

    if (!completionCtrl) {
      std::optional<unsigned> uniquePred = getAnnotatedExitPredBB(endOp);
      if (!uniquePred)
        return (void)(func.emitOpError()
                      << "failed to derive completion control for exit BB #"
                      << exitBlockID
                      << ": exit BB has no discoverable control token and missing required '"
                      << EXIT_PRED_BB_ATTR_NAME
                      << "' annotation (refusing to guess predecessor)" );

      completionCtrl = recoverExitCompletionTokenFromPredBB(func, *uniquePred);
      if (!completionCtrl)
        return (void)(func.emitOpError()
                      << "failed to derive completion control for exit BB #"
                      << exitBlockID
               << ": exit BB has no discoverable control token and could not uniquely "
                 "recover a sunk exit control from predecessor BB #"
                      << *uniquePred);
    }

    // Nothing to do if it already matches the existing operand.
    if (completionCtrl == originalCompletion)
      return;

    OpBuilder builder(endOp);
    builder.setInsertionPoint(endOp);
    auto joinOp = builder.create<handshake::JoinOp>(
        endOp.getLoc(), ValueRange{completionCtrl, originalCompletion});
    joinOp->setAttr(COMPLETION_JOIN_ATTR_NAME, builder.getUnitAttr());
    if (auto bbAttr = endOp->getAttr(BB_ATTR_NAME))
      joinOp->setAttr(BB_ATTR_NAME, bbAttr);
    endOp->setOperand(lastIdx, joinOp.getResult());
  }

  /// Walks every control edge in `func` inserting transparent `handshake.sched_cp`
  /// probes and tagging them with block metadata.
  void doInsert(handshake::FuncOp func, uint32_t &nextSchedId) {
    for (mlir::Block &block : func.getBody()) {
      for (mlir::Operation &operation : block) {
        // Skip the sched_cp ops we introduce ourselves to avoid reinstrumenting
        // the same edge.
        if (mlir::isa<handshake::SchedCPOp>(&operation))
          continue;

        if (auto joinOp = mlir::dyn_cast<handshake::JoinOp>(&operation))
          if (joinOp->hasAttr(COMPLETION_JOIN_ATTR_NAME))
            continue;

        llvm::SmallVector<mlir::OpOperand *> operandsToInstrument;
        operandsToInstrument.reserve(operation.getNumOperands());
        for (mlir::OpOperand &operand : operation.getOpOperands()) {
          if (!mlir::isa<handshake::ControlType>(operand.get().getType()))
            continue;

          mlir::Operation *def = operand.get().getDefiningOp();
          if (def && mlir::isa<handshake::SchedCPOp>(def))
            continue;

          operandsToInstrument.push_back(&operand);
        }

        if (operandsToInstrument.empty())
          continue;

        mlir::OpBuilder builder(&operation);
        builder.setInsertionPoint(&operation);

        for (mlir::OpOperand *operand : operandsToInstrument) {
          mlir::Value input = operand->get();

          BBEndpoints endpoints;
          if (!dynamatic::getBBEndpoints(input, &operation, endpoints))
            continue;

          bool isBackedge = false;
          bool fromCompletionJoin = false;
          if (auto joinSrc = input.getDefiningOp<handshake::JoinOp>())
            fromCompletionJoin =
                joinSrc->hasAttr(COMPLETION_JOIN_ATTR_NAME) &&
                llvm::isa<handshake::EndOp>(&operation);

          if (!fromCompletionJoin && endpoints.srcBB == endpoints.dstBB) {
            if (!dynamatic::isBackedge(input, &operation, &endpoints))
              continue;
            isBackedge = true;
          }

          auto schedCP = builder.create<handshake::SchedCPOp>(
              operation.getLoc(), input.getType(), input);

          auto idType = builder.getIntegerType(32, /*isSigned=*/false);
          auto idAttr =
              builder.getIntegerAttr(idType, static_cast<uint64_t>(nextSchedId++));
          schedCP->setAttr(handshake::SchedCPOp::SCHED_ID_ATTR_NAME, idAttr);

          schedCP->setAttr(handshake::SchedCPOp::SRC_BB_ATTR_NAME,
                           builder.getI32IntegerAttr(static_cast<int32_t>(
                               endpoints.srcBB)));
          schedCP->setAttr(handshake::SchedCPOp::DST_BB_ATTR_NAME,
                           builder.getI32IntegerAttr(static_cast<int32_t>(
                               endpoints.dstBB)));

          if (isBackedge)
            schedCP->setAttr(handshake::SchedCPOp::BACKEDGE_ATTR_NAME,
                             builder.getBoolAttr(true));

          // Prefer to inherit the logical BB from the consuming op, but fall
          // back to the producer when needed to preserve metadata.
          // if (!dynamatic::inheritBB(&operation, schedCP))
          //  dynamatic::inheritBBFromValue(input, schedCP);

          operand->set(schedCP.getResult());
        }
      }
    }
  }
};

} // namespace

std::unique_ptr<dynamatic::DynamaticPass>
dynamatic::createHandshakeInsertSchedCP() {
  return std::make_unique<HandshakeInsertSchedCPPass>();
}
