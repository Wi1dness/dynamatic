/// Include the header we just created.
#include "dynamatic/Transforms/HandshakeMuxToMerge.h"

/// Include some other useful headers.
#include "dynamatic/Dialect/Handshake/HandshakeOps.h"
#include "dynamatic/Support/CFG.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace dynamatic;

namespace {

/// Rewrite pattern that will match on all muxes in the IR and replace each of
/// them with a merge taking the same inputs (except the `select` input which
/// merges do not have due to their undeterministic nature).
struct ReplaceMuxWithMerge : public OpRewritePattern<handshake::MuxOp> {
  using OpRewritePattern<handshake::MuxOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(handshake::MuxOp muxOp,
                                PatternRewriter &rewriter) const override {
    // Retrieve all mux inputs except the `select`
    ValueRange dataOperands = muxOp.getDataOperands();
    // Create a merge in the IR at the mux's position and with the same data
    // inputs (or operands, in MLIR jargon)
    handshake::MergeOp mergeOp =
        rewriter.create<handshake::MergeOp>(muxOp.getLoc(), dataOperands);
    // Make the merge part of the same basic block (BB) as the mux
    inheritBB(muxOp, mergeOp);
    // Retrieve the merge's output (or result, in MLIR jargon)
    Value mergeResult = mergeOp.getResult();
    // Replace usages of the mux's output with the new merge's output
    rewriter.replaceOp(muxOp, mergeResult);
    // Signal that the pattern succeeded in rewriting the mux
    return success();
  }
};


/// Simple driver for the pass that replaces all muxes with merges.
struct HandshakeMuxToMergePass
    : public dynamatic::impl::HandshakeMuxToMergeBase<HandshakeMuxToMergePass> {

  void runDynamaticPass() override {
    // This is the top-level operation in all MLIR files. All the IR is nested
    // within it
    mlir::ModuleOp mod = getOperation();
    MLIRContext *ctx = &getContext();

    // Define the set of rewrite patterns we want to apply to the IR
    RewritePatternSet patterns(ctx);
    patterns.add<ReplaceMuxWithMerge>(ctx);

    // Run a greedy pattern rewriter on the entire IR under the top-level module
    // operation
    mlir::GreedyRewriteConfig config;
    if (failed(applyPatternsAndFoldGreedily(mod, std::move(patterns), config))) {
      // If the greedy pattern rewriter fails, the pass must also fail
      return signalPassFailure();
    }
  };
};
}; // namespace

/// Implementation of our pass constructor, which just returns an instance of
/// the `HandshakeMuxToMergePass` struct. 
std::unique_ptr<dynamatic::DynamaticPass>
dynamatic::createHandshakeMuxToMerge() {
  return std::make_unique<HandshakeMuxToMergePass>();
}
