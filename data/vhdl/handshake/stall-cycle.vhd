library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity stall is
  generic (
    DATA_TYPE : integer;
    CFG_TYPE  : integer;
    STALL_ID  : integer
  );
  port (
    clk, rst : in std_logic;

    -- data input channel
    ins       : in  std_logic_vector(DATA_TYPE - 1 downto 0);
    ins_valid : in  std_logic;
    ins_ready : out std_logic;

    -- configuration input channel (broadcast)
    cfg       : in  std_logic_vector(CFG_TYPE - 1 downto 0);
    cfg_valid : in  std_logic;
    cfg_ready : out std_logic;

    -- data output channel
    outs       : out std_logic_vector(DATA_TYPE - 1 downto 0);
    outs_valid : out std_logic;
    outs_ready : in  std_logic
  );
end entity;

architecture arch of stall is
  constant FIELD_W : integer := 32;

  -- 32-bit Galois LFSR tap mask for polynomial x^32 + x^22 + x^2 + x^1 + 1.
  -- Canonical implementation: if (lsb==1) lfsr = (lfsr >> 1) ^ 0x80200003; else lfsr >>= 1;
  constant LFSR_TAPS_MASK : unsigned(FIELD_W - 1 downto 0) := x"80200003";

  -- CFG payload layout (LSB-first):
  --   [31:0]   seed
  --   [63:32]  threshold
  --   [95:64]  base
  --   [127:96] id
  signal cfg_seed      : std_logic_vector(FIELD_W - 1 downto 0);
  signal cfg_threshold : std_logic_vector(FIELD_W - 1 downto 0);
  signal cfg_id        : std_logic_vector(FIELD_W - 1 downto 0);
  signal cfg_match     : std_logic;
  signal cfg_fire      : std_logic;

  signal seed_reg      : unsigned(FIELD_W - 1 downto 0) := (others => '1');
  signal threshold_reg : unsigned(FIELD_W - 1 downto 0) := (others => '0');

  -- 32-bit LFSR state.
  signal lfsr_state : unsigned(FIELD_W - 1 downto 0) := (others => '1');
  signal lfsr_next  : unsigned(FIELD_W - 1 downto 0);

  -- Stall control: per-cycle decision.
  signal inflight       : std_logic;
  signal stall_decision : std_logic;
  signal allow          : std_logic;

  function get_cfg_field(vec : std_logic_vector; offset : integer)
    return std_logic_vector is
    variable outv : std_logic_vector(FIELD_W - 1 downto 0) := (others => '0');
  begin
    for i in 0 to FIELD_W - 1 loop
      if (offset + i) >= 0 and (offset + i) < vec'length then
        outv(i) := vec(offset + i);
      end if;
    end loop;
    return outv;
  end function;

  function to_slv32(x : integer) return std_logic_vector is
  begin
    return std_logic_vector(to_unsigned(x, FIELD_W));
  end function;

begin
  -- =======================================================================
  -- Configuration (broadcast)
  -- =======================================================================

  -- Always accept cfg broadcasts (non-blocking).
  cfg_ready <= '1';

  -- CFG field extraction is bounds-safe even if CFG_TYPE < 128.
  cfg_seed      <= get_cfg_field(cfg, 0);
  cfg_threshold <= get_cfg_field(cfg, 32);
  cfg_id        <= get_cfg_field(cfg, 96);

  cfg_match <= '1' when cfg_id = to_slv32(STALL_ID) else '0';
  cfg_fire  <= '1' when (cfg_valid = '1' and cfg_match = '1') else '0';

  cfg_regs : process(clk)
  begin
    if rising_edge(clk) then
      -- Configuration is only modified through the cfg bus.
      -- Assumption: configuration broadcasts complete before asserting reset,
      -- so seed_reg/threshold_reg/base_reg are stable while rst=1.
      if cfg_fire = '1' then
        seed_reg      <= unsigned(cfg_seed);
        threshold_reg <= unsigned(cfg_threshold);
      end if;
    end if;
  end process;

  -- =======================================================================
  -- LFSR (randomness source)
  -- =======================================================================

  -- 32-bit Galois LFSR next-state logic (see LFSR_TAPS_MASK above).
  lfsr_next <= (shift_right(lfsr_state, 1) xor LFSR_TAPS_MASK) when lfsr_state(0) = '1'
              else shift_right(lfsr_state, 1);

  lfsr_regs : process(clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        -- Reset LFSR to the configured seed.
        lfsr_state <= seed_reg;
      else
        -- Per-cycle stepping: advance the LFSR every cycle.
        lfsr_state <= lfsr_next;
      end if;
    end if;
  end process;

  -- =======================================================================
  -- Handshake gating + stall control
  -- =======================================================================

  process(clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        inflight <= '0';
      elsif (stall_decision = '0') then
        inflight <= outs_valid and (not outs_ready);
      else
        inflight <= inflight and ins_valid and (not outs_ready);
      end if;
    end if;
  end process;

  -- Per-cycle probabilistic stall decision.
  -- When there is a pending transfer request (ins_valid=1), we block the
  -- handshake for the current cycle if the (current) LFSR sample is below the
  -- configured threshold.
  stall_decision <= '1' when (ins_valid = '1' and lfsr_state < threshold_reg) else '0';
  allow <= not stall_decision;

  -- Non-buffering stall gate: when blocked, prevent transfers by deasserting
  -- both valid and ready.
  outs       <= ins;
  outs_valid <= ins_valid when (allow or inflight) else '0';
  ins_ready  <= outs_ready when (allow or inflight) else '0';

end architecture;
