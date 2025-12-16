from generators.support.utils import data
from generators.support.signal_manager.utils.entity import generate_entity


def generate_strip_extra_signal(name, params):
    bitwidth = int(params.get("bitwidth", 0))
    extra_signals = params.get("extra_signals", {}) or {}

    if extra_signals:
        return _generate_with_signal_manager(name, bitwidth, extra_signals)

    return _generate_strip_extra_signal(name, bitwidth)


def _generate_strip_extra_signal(name, bitwidth):
    input_data = data("ins          : in std_logic_vector({bitwidth} - 1 downto 0);".format(bitwidth=bitwidth), bitwidth)
    output_data = data("outs         : out std_logic_vector({bitwidth} - 1 downto 0);".format(bitwidth=bitwidth), bitwidth)
    data_assign = data("outs <= ins;", bitwidth)

    entity = f"""
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- Entity of strip extra signal
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
-- Architecture of strip extra signal
architecture arch of {name} is
begin
  {data_assign}
  outs_valid <= ins_valid;
  ins_ready  <= outs_ready;
end architecture;
"""

    return entity + architecture


def _generate_with_signal_manager(name, bitwidth, extra_signals):
    inner_name = f"{name}_inner"
    inner = _generate_strip_extra_signal(inner_name, bitwidth)

    entity = generate_entity(name, [{
        "name": "ins",
        "bitwidth": bitwidth,
        "extra_signals": extra_signals
    }], [{
        "name": "outs",
        "bitwidth": bitwidth,
        "extra_signals": {}
    }])

    port_map_entries = [
        ("clk", "clk"),
        ("rst", "rst"),
    ]
    if bitwidth:
        port_map_entries.append(("ins", "ins"))
        port_map_entries.append(("outs", "outs"))
    port_map_entries.append(("ins_valid", "ins_valid"))
    port_map_entries.append(("ins_ready", "ins_ready"))
    port_map_entries.append(("outs_ready", "outs_ready"))
    port_map_entries.append(("outs_valid", "outs_valid"))

    port_map_lines = []
    for index, (port, signal) in enumerate(port_map_entries):
        suffix = "," if index < len(port_map_entries) - 1 else ""
        port_map_lines.append(f"        {port:<11}=> {signal}{suffix}")

    port_map_body = "\n".join(port_map_lines)

    architecture = f"""
-- Architecture of strip extra signal signal manager
architecture arch of {name} is
begin
    inner : entity work.{inner_name}(arch)
    port map(
{port_map_body}
    );
end architecture;
"""

    return inner + entity + architecture
