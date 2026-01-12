//===- HandshakeInsertMemStalls.cpp - Insert handshake.stall ops -*- C++ -*-===//
//
// Inserts `handshake.stall` operations on load/store memory channels.
//
// NOTE: This is the Handshake-level instrumentation pass (Route C1).
// It is intentionally focused on IR rewrites + metadata emission; RTL semantics
// of `handshake.stall` will be handled in the Handshake->HW lowering.
//
//===----------------------------------------------------------------------===//

#include "dynamatic/Transforms/HandshakeInsertMemStalls.h"

#include "dynamatic/Dialect/Handshake/HandshakeOps.h"
#include "dynamatic/Dialect/Handshake/HandshakeTypes.h"
#include "dynamatic/Support/LLVM.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>

using namespace mlir;
using namespace dynamatic;

namespace {

namespace ljson = llvm::json;

static constexpr llvm::StringLiteral STALL_ID_ATTR_NAME =
    handshake::StallOp::STALL_ID_ATTR_NAME;

struct StallPointInfo {
  uint32_t id;
  std::string kind;    // e.g., "top_arg", "top_res", "load_addr", ...
  std::string opName;  // producer/consumer operation name when available
  std::string channel; // arg/res name when applicable
};

static std::string getHandshakeName(Operation *op) {
  if (!op)
    return "";
  if (auto s = op->getAttrOfType<StringAttr>("handshake.name"))
    return s.str();
  return op->getName().getStringRef().str();
}

static handshake::StallOp insertStallOnValue(OpBuilder &b, Location loc, Value v,
                                             Value cfg, uint32_t id) {
  auto stall = b.create<handshake::StallOp>(loc, v.getType(), v, cfg);
  stall->setAttr(STALL_ID_ATTR_NAME, b.getI32IntegerAttr(id));
  return stall;
}

static void recordStallPoint(SmallVectorImpl<StallPointInfo> &points,
                             uint32_t id, StringRef kind, StringRef opName,
                             StringRef channel) {
  points.push_back(StallPointInfo{id, kind.str(), opName.str(), channel.str()});
}

static void writeStallPointsJson(StringRef path, StringRef dutName,
                                 ArrayRef<StallPointInfo> points) {
  if (path.empty())
    return;

  ljson::Array fields;
  fields.push_back(ljson::Object{{"name", "seed"}, {"width", 32}});
  fields.push_back(ljson::Object{{"name", "threshold"}, {"width", 32}});
  fields.push_back(ljson::Object{{"name", "base"}, {"width", 32}});

  ljson::Array sp;
  for (const auto &p : points) {
    ljson::Object o;
    o["id"] = static_cast<int64_t>(p.id);
    o["op_kind"] = p.kind;
    if (!p.opName.empty())
      o["op_name"] = p.opName;
    if (!p.channel.empty())
      o["channel"] = p.channel;
    sp.push_back(std::move(o));
  }

  ljson::Object root;
  root["dut"] = dutName.str();
  root["num_stall_points"] = static_cast<int64_t>(points.size());
  root["param_format"] = ljson::Object{{"fields", std::move(fields)}};
  root["stall_points"] = std::move(sp);

  std::error_code ec;
  llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    llvm::errs() << "failed to open stall points json '" << path
                 << "': " << ec.message() << "\n";
    return;
  }
  os << ljson::Value(std::move(root)) << "\n";
}

class HandshakeInsertMemStallsPass
    : public dynamatic::impl::HandshakeInsertMemStallsBase<
          HandshakeInsertMemStallsPass> {
public:
  using dynamatic::impl::HandshakeInsertMemStallsBase<
      HandshakeInsertMemStallsPass>::HandshakeInsertMemStallsBase;

  void runDynamaticPass() override {
    ModuleOp mod = getOperation();

    // Use the first internal handshake.func name as the DUT name when
    // emitting metadata.
    std::string dutName = "dut";
    for (auto func : mod.getOps<handshake::FuncOp>()) {
      if (!func.isExternal()) {
        dutName = func.getName().str();
        break;
      }
    }

    MLIRContext *ctx = &getContext();
    OpBuilder b(ctx);

    // We assign IDs by walk order; stable enough for phase1 as long as the
    // IR remains deterministic for a given kernel+pipeline.
    uint32_t nextId = 0;
    SmallVector<StallPointInfo> allPoints;

    auto ensureCfgArg = [&](handshake::FuncOp func) -> BlockArgument {
      Block &entry = func.getBody().front();
      // Add cfg as last argument: !handshake.channel<i128>
      auto cfgType = handshake::ChannelType::get(IntegerType::get(ctx, 128));

      // If already present (by name), reuse.
      if (auto names = func.getArgNames()) {
        for (auto it : llvm::enumerate(names)) {
          if (auto s = dyn_cast<StringAttr>(it.value()); s && s == "cfg")
            return entry.getArgument(it.index());
        }
      }

      // Update function type + argNames attribute.
      SmallVector<Type> argTypes(func.getArgumentTypes());
      argTypes.push_back(cfgType);
      auto newType = FunctionType::get(ctx, argTypes, func.getResultTypes());
      func.setFunctionTypeAttr(TypeAttr::get(newType));

      SmallVector<Attribute> argNames;
      if (auto names = func.getArgNames())
        argNames.append(names.begin(), names.end());
      argNames.push_back(StringAttr::get(ctx, "cfg"));
      func->setAttr("argNames", ArrayAttr::get(ctx, argNames));

      // Add block argument.
      entry.addArgument(cfgType, func.getLoc());
      return entry.getArguments().back();
    };

    auto instrumentFunc = [&](handshake::FuncOp func) {
      if (func.isExternal())
        return;

      Block &entry = func.getBody().front();
      BlockArgument cfgArg = ensureCfgArg(func);
      Value cfg = cfgArg;

      // 1) Stall on top-level scalar IO: any !handshake.channel<...> argument/result
      //    except cfg.
      for (BlockArgument arg : entry.getArguments()) {
        if (arg == cfgArg)
          continue;
        if (!isa<handshake::ChannelType>(arg.getType()))
          continue;

        b.setInsertionPointToStart(&entry);
        Value original = arg;
        uint32_t id = nextId++;
        auto stallOp = insertStallOnValue(b, func.getLoc(), original, cfg, id);
        Value stalled = stallOp.getResult();

        // Replace all uses of the block argument *except* the newly created
        // stall itself.
        arg.replaceAllUsesExcept(stalled, stallOp.getOperation());

        std::string argName;
        if (auto names = func.getArgNames())
          argName = cast<StringAttr>(names[arg.getArgNumber()]).str();

        recordStallPoint(allPoints, id, "top_arg", func.getName(), argName);
      }

      auto end = cast<handshake::EndOp>(entry.getTerminator());
      b.setInsertionPoint(end);
      for (auto it : llvm::enumerate(end->getOperands())) {
        Value v = it.value();
        if (!isa<handshake::ChannelType>(v.getType()))
          continue;
        uint32_t id = nextId++;
        Value stalled = insertStallOnValue(b, func.getLoc(), v, cfg, id).getResult();
        end->setOperand(it.index(), stalled);

        std::string resName;
        if (auto names = func.getResNames())
          resName = cast<StringAttr>(names[it.index()]).str();
        recordStallPoint(allPoints, id, "top_res", func.getName(), resName);
      }

      // 2) Stall on load/store memory channels.
      // We *avoid* inserting stalls on the memory-interface boundary ports
      // (load.addrOut/load.dataFromMem/store.addrOut/store.dataToMem) because
      // MemoryOpInterface verifiers expect a very specific structure.
      // Instead, we insert stalls on the dataflow side of memory ops.

      // 2a) For load: stall addrIn coming from dataflow (operand #0) and dataOut going to dataflow (result #1).
      for (auto load : func.getOps<handshake::LoadOp>()) {
        b.setInsertionPoint(load);
        std::string loadName = getHandshakeName(load);

        uint32_t addrId = nextId++;
        Value addrIn = load.getOperand(0);
        auto addrStall = insertStallOnValue(b, load.getLoc(), addrIn, cfg, addrId);
        load.setOperand(0, addrStall.getResult());

        recordStallPoint(allPoints, addrId, "load_addrin", loadName, "");

        uint32_t dataId = nextId++;
        Value dataOut = load.getResult(1);
        auto dataStall = insertStallOnValue(b, load.getLoc(), dataOut, cfg, dataId);
        load.getResult(1).replaceAllUsesExcept(dataStall.getResult(),
                         dataStall.getOperation());

        recordStallPoint(allPoints, dataId, "load_dataout", loadName, "");
      }

      // 2b) For store: stall addrIn/dataIn operands (operands #0/#1), coming from dataflow.
      for (auto store : func.getOps<handshake::StoreOp>()) {
        b.setInsertionPoint(store);
        std::string storeName = getHandshakeName(store);
        for (unsigned opIdx = 0; opIdx < 2; ++opIdx) {
          uint32_t id = nextId++;
          Value in = store.getOperand(opIdx);
          auto stallOp = insertStallOnValue(b, store.getLoc(), in, cfg, id);
          store.setOperand(opIdx, stallOp.getResult());

          recordStallPoint(allPoints, id,
                           opIdx == 0 ? "store_addrin" : "store_datain",
                           storeName, "");
        }
      }
    };

    for (auto func : mod.getOps<handshake::FuncOp>())
      instrumentFunc(func);

    writeStallPointsJson(stallPointsJson, /*dutName=*/dutName, allPoints);
  }
};

} // namespace

std::unique_ptr<dynamatic::DynamaticPass>
dynamatic::createHandshakeInsertMemStalls() {
  return std::make_unique<HandshakeInsertMemStallsPass>();
}
