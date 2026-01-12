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

  -- Use low LFSR bits as small random jitter added to base_reg.
  constant JITTER_W : integer := 3;

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
  signal cfg_base      : std_logic_vector(FIELD_W - 1 downto 0);
  signal cfg_id        : std_logic_vector(FIELD_W - 1 downto 0);
  signal cfg_match     : std_logic;
  signal cfg_fire      : std_logic;

  signal seed_reg      : unsigned(FIELD_W - 1 downto 0) := (others => '1');
  signal threshold_reg : unsigned(FIELD_W - 1 downto 0) := (others => '0');
  signal base_reg      : unsigned(FIELD_W - 1 downto 0) := (others => '0');

  -- 32-bit LFSR state.
  signal lfsr_state : unsigned(FIELD_W - 1 downto 0) := (others => '1');
  signal lfsr_next  : unsigned(FIELD_W - 1 downto 0);
  signal lfsr_advance : std_logic;
  signal jitter_u     : unsigned(FIELD_W - 1 downto 0);

  -- Stall control.
  signal stall_cnt    : unsigned(FIELD_W - 1 downto 0) := (others => '0');
  signal stall_active : std_logic;
  signal stall_decision   : std_logic;
  signal stall_len_u      : unsigned(FIELD_W - 1 downto 0);
  signal fire_allowed     : std_logic;
  signal allow        : std_logic;

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
  cfg_base      <= get_cfg_field(cfg, 64);
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
        base_reg      <= unsigned(cfg_base);
      end if;
    end if;
  end process;

  -- =======================================================================
  -- LFSR (randomness source)
  -- =======================================================================

  -- 32-bit Galois LFSR next-state logic (see LFSR_TAPS_MASK above).
  lfsr_next <= (shift_right(lfsr_state, 1) xor LFSR_TAPS_MASK) when lfsr_state(0) = '1'
              else shift_right(lfsr_state, 1);

  -- Advance the LFSR once per successful handshake when transfers are allowed.
  lfsr_advance <= fire_allowed;

  -- Random jitter (0..2^JITTER_W-1) derived from low bits.
  -- We derive it from lfsr_next so that the stall decision/length computed
  -- at fire_allowed corresponds to the post-advance LFSR sample.
  jitter_u <= resize(lfsr_next(JITTER_W - 1 downto 0), FIELD_W);

  lfsr_regs : process(clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        -- Reset LFSR to the configured seed.
        lfsr_state <= seed_reg;
      else
        if lfsr_advance = '1' then
          lfsr_state <= lfsr_next;
        end if;
      end if;
    end if;
  end process;

  -- =======================================================================
  -- Handshake gating + stall control
  -- =======================================================================

  stall_active <= '1' when stall_cnt /= 0 else '0';
  allow <= not stall_active;

  -- Successful handshake when transfers are allowed.
  fire_allowed <= '1' when (ins_valid = '1' and outs_ready = '1' and allow = '1') else '0';

  -- Decision and duration are evaluated at the same time as a successful
  -- allowed transfer.
  -- The decision corresponds to lfsr_next (the value we advance to at the
  -- same clock edge), not the pre-advance lfsr_state.
  stall_decision <= '1' when (lfsr_next < threshold_reg) else '0';
  stall_len_u <= base_reg + jitter_u;

  -- Non-buffering stall gate: when blocked, prevent transfers by deasserting
  -- both valid and ready.
  outs       <= ins;
  outs_valid <= ins_valid and allow;
  ins_ready  <= outs_ready and allow;

  stall_regs : process(clk)
    variable init_jitter : unsigned(FIELD_W - 1 downto 0);
    variable init_len    : unsigned(FIELD_W - 1 downto 0);
  begin
    if rising_edge(clk) then
      if rst = '1' then
        -- Initialize internal state from the configured registers while rst=1.
        -- We compute the *initial* stall decision/length from current seed_reg,
        -- threshold_reg and base_reg, so that after reset the unit starts in a
        -- state consistent with the configuration.

        init_jitter := resize(seed_reg(JITTER_W - 1 downto 0), FIELD_W);
        init_len := base_reg + init_jitter;
        if (seed_reg < threshold_reg) and (init_len /= 0) then
          stall_cnt <= init_len;
        else
          stall_cnt <= (others => '0');
        end if;
      else
        -- Decrement stall counter only when there is an actual pending
        -- transfer request (ins_valid=1). No request => no countdown.
        if stall_cnt /= 0 and ins_valid = '1' then
          stall_cnt <= stall_cnt - 1;
        end if;

        -- After a successful allowed handshake, decide whether to inject a
        -- stall before the next transfer.
        if fire_allowed = '1' then
          if stall_decision = '1' and stall_len_u /= 0 then
            stall_cnt <= stall_len_u;
          else
            stall_cnt <= (others => '0');
          end if;
        end if;
      end if;
    end if;
  end process;

end architecture;
