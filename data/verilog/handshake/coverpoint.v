`timescale 1ns/1ps
module coverpoint #(
  parameter integer DATA_TYPE = 32,
  parameter integer COVERPOINT_ID = 0
) (
  input  clk,
  input  rst,
  // Input channel
  input  [DATA_TYPE - 1 : 0] ins,
  input  ins_valid,
  output ins_ready,
  // Output channel
  output [DATA_TYPE - 1 : 0] outs,
  output outs_valid,
  input  outs_ready
);
  // Keep COVERPOINT_ID in scope to avoid unused parameter warnings.
  localparam integer _coverpoint_id_anchor = COVERPOINT_ID;

  // Channel is covered if both valid and ready assert
  reg covered;
  always @(posedge clk or posedge rst) begin
    if (rst) begin
      covered <= 0;
    end else if (outs_valid & outs_ready) begin
      covered <= 1;
    end else begin
      covered <= covered;
    end
  end

  // Forward channel signals unchanged.
  assign outs_valid = ins_valid;
  assign ins_ready = outs_ready;
  assign outs = ins;

endmodule
