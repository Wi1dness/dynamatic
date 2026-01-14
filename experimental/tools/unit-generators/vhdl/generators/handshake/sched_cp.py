from generators.support.signal_manager.utils.entity import generate_entity
from generators.support.signal_manager.utils.generation import generate_default_mappings
from generators.support.utils import data


def generate_sched_cp(name, params):
    bitwidth = int(params.get("bitwidth", 0))
    extra_signals = params.get("extra_signals", {}) or {}
    schedcp_id = int(params.get("schedcp_id", 0))
    bitmap_lg2 = int(params.get("bitmap_lg2", 16))

    if bitmap_lg2 < 1:
        raise ValueError("bitmap_lg2 must be >= 1")

    if "schedcp_ts" not in extra_signals:
        raise ValueError("sched_cp requires schedcp_ts extra signal")

    timestamp_width = int(extra_signals["schedcp_ts"])
    if timestamp_width <= 0:
        raise ValueError("sched_cp requires schedcp_ts extra signal with positive width")

    covsum_width = int(extra_signals.get("schedcp_covsum", 32))
    if covsum_width <= 0:
        raise ValueError(
            "sched_cp requires schedcp_covsum extra signal with positive width"
        )
    if covsum_width <= bitmap_lg2:
        raise ValueError(
            "schedcp_covsum width must exceed bitmap_lg2 to encode unique hits"
        )
    has_covsum = "schedcp_covsum" in extra_signals

    return _generate_with_signal_manager(
        name,
        bitwidth,
        extra_signals,
        schedcp_id,
        bitmap_lg2,
        has_covsum,
        covsum_width,
        timestamp_width
    )

def _generate_sched_cp(name, bitwidth, schedcp_id):
    input_data = data(
        f"ins          : in std_logic_vector({bitwidth} - 1 downto 0);", bitwidth)
    output_data = data(
        f"outs         : out std_logic_vector({bitwidth} - 1 downto 0);", bitwidth)
    data_assignment = "outs <= ins;" if bitwidth else ""

    entity = f"""
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- Entity of sched cp
entity {name} is
  port (
    clk          : in std_logic;
    rst          : in std_logic;
    {input_data}
    ins_valid    : in std_logic;
    outs_ready   : in std_logic;
    {output_data}
    outs_valid   : out std_logic;
    ins_ready    : out std_logic
  );
end entity;
"""

    architecture = f"""
-- Architecture of sched cp
architecture arch of {name} is
  constant SCHEDCP_ID_ANCHOR : integer := {schedcp_id};
begin
  {data_assignment}
  outs_valid <= ins_valid;
  ins_ready  <= outs_ready;
end architecture;
"""

    return entity + architecture


def _generate_with_signal_manager(
    name,
    bitwidth,
    extra_signals,
    schedcp_id,
    bitmap_lg2,
    has_covsum,
    covsum_width,
    timestamp_width
):
    inner_name = f"{name}_inner"
    inner = _generate_sched_cp(inner_name, bitwidth, schedcp_id)

    in_channels = [
        {
            "name": "ins",
            "bitwidth": bitwidth,
            "extra_signals": extra_signals,
        }
    ]
    out_channels = [
        {
            "name": "outs",
            "bitwidth": bitwidth,
            "extra_signals": extra_signals,
        }
    ]

    entity = generate_entity(name, in_channels, out_channels)

    forwarding_lines = [
        f"  outs_{signal_name} <= ins_{signal_name};"
        for signal_name in extra_signals
        if signal_name not in {"schedcp_covsum", "schedcp_ts"}
    ]
    forwarding_body = "\n".join(forwarding_lines)
    if forwarding_body:
        forwarding_body += "\n"

    port_mappings = generate_default_mappings(in_channels + out_channels)

    if has_covsum:
        covsum_signal_decl = (
            "    signal covsum_in    : unsigned(COVSUM_WIDTH - 1 downto 0);\n"
            "    signal covsum_bump  : unsigned(COVSUM_WIDTH - 1 downto 0);\n"
            "    signal covsum_next  : unsigned(COVSUM_WIDTH - 1 downto 0);\n"
        )
        covsum_comb_logic = (
            "    covsum_in   <= resize(unsigned(ins_schedcp_covsum), COVSUM_WIDTH);\n"
            "    covsum_bump <= to_unsigned(1, COVSUM_WIDTH) when (handshake_fire = '1' and hit_before = '0') else (others => '0');\n"
            "    covsum_next <= covsum_in + covsum_bump;\n"
            "    outs_schedcp_covsum <= std_logic_vector(covsum_next);\n"
        )
    else:
        covsum_signal_decl = ""
        covsum_comb_logic = ""

    covsum_proc_update = (
        "                if hit_before = '0' then\n"
        "                    covsum_reg <= covsum_reg + to_unsigned(1, covsum_reg'length);\n"
        "                end if;\n"
    )

    architecture = f"""
-- Architecture of sched cp signal manager
architecture arch of {name} is
    constant TIMESTAMP_WIDTH : natural := {timestamp_width};
    constant BITMAP_LG2 : natural := {bitmap_lg2};
    constant BITMAP_SIZE : natural := 2 ** BITMAP_LG2;
    constant COVSUM_WIDTH : natural := {covsum_width};

    function rotate_left(value : unsigned; amount : natural) return unsigned is
        constant LEN : natural := value'length;
        variable shift_amt : natural := 0;
        variable result : unsigned(value'range) := value;
    begin
        if LEN = 0 then
            return value;
        end if;

        shift_amt := amount mod LEN;
        if shift_amt = 0 then
            return value;
        end if;

        result :=
            value(value'high - shift_amt downto value'low) &
            value(value'high downto value'high - shift_amt + 1);
        return result;
    end function;

    signal cycle_counter    : unsigned(TIMESTAMP_WIDTH - 1 downto 0) := (others => '0');
    signal last_start_cycle : unsigned(TIMESTAMP_WIDTH - 1 downto 0) := (others => '0');
    signal activation_count : unsigned(TIMESTAMP_WIDTH - 1 downto 0) := (others => '0');
    signal cov_bitmap       : std_logic_vector(BITMAP_SIZE - 1 downto 0) := (others => '0');
    signal covsum_reg       : unsigned(COVSUM_WIDTH - 1 downto 0) := (others => '0');

    signal handshake_fire   : std_logic;
    signal start_cycle_val  : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal interval_val     : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal latency_val      : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal rotated_interval : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal rotated_count    : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal hash_val         : unsigned(TIMESTAMP_WIDTH - 1 downto 0);
    signal hash_index       : std_logic_vector(BITMAP_LG2 - 1 downto 0);
    signal hash_index_int   : integer range 0 to BITMAP_SIZE - 1;
    signal hit_before       : std_logic;
{covsum_signal_decl}begin
{forwarding_body}  inner : entity work.{inner_name}(arch)
    port map(
        clk => clk,
        rst => rst,
        {port_mappings}
    );

    handshake_fire <= '1' when (ins_valid = '1' and outs_ready = '1') else '0';
    start_cycle_val <= resize(unsigned(ins_schedcp_ts), TIMESTAMP_WIDTH);
    interval_val    <= (others => '0') when (activation_count = to_unsigned(1, activation_count'length))
                      else (start_cycle_val - last_start_cycle);
    latency_val     <= cycle_counter - start_cycle_val;
    rotated_interval <= rotate_left(interval_val, 1);
    rotated_count    <= rotate_left(activation_count, 2);
    hash_val         <= rotated_interval xor rotated_count xor latency_val;
    hash_index       <= std_logic_vector(hash_val(BITMAP_LG2 - 1 downto 0));
    hash_index_int   <= to_integer(unsigned(hash_index));
    hit_before       <= cov_bitmap(hash_index_int);
{covsum_comb_logic}

    outs_schedcp_ts <= std_logic_vector(cycle_counter);

    process(clk, rst)
        variable bitmap_next      : std_logic_vector(BITMAP_SIZE - 1 downto 0);
    begin
        if rst = '1' then
            cycle_counter    <= (others => '0');
            last_start_cycle <= (others => '0');
            activation_count <= (others => '0');
            activation_count(0) <= '1';
        elsif rising_edge(clk) then
            cycle_counter <= cycle_counter + to_unsigned(1, cycle_counter'length);

            if handshake_fire = '1' then
                bitmap_next := cov_bitmap;
                bitmap_next(hash_index_int) := '1';
                cov_bitmap <= bitmap_next;
{covsum_proc_update}
                last_start_cycle <= start_cycle_val;
                activation_count <= activation_count + to_unsigned(1, activation_count'length);
            end if;
        end if;
    end process;
end architecture;
"""

    return inner + entity + architecture
