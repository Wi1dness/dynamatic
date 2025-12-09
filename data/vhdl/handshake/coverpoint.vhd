library ieee;
use ieee.std_logic_1164.all;

entity coverpoint is
  generic (
    DATA_TYPE  : integer;
    COVERPOINT_ID : integer := 0
  );
  port (
    clk, rst : in std_logic;
    -- input channel
    ins       : in  std_logic_vector(DATA_TYPE - 1 downto 0);
    ins_valid : in  std_logic;
    ins_ready : out std_logic;
    -- output channel
    outs       : out std_logic_vector(DATA_TYPE - 1 downto 0);
    outs_valid : out std_logic;
    outs_ready : in  std_logic
  );
end entity coverpoint;

architecture arch of coverpoint is
  constant COVERPOINT_ID_ANCHOR : integer := COVERPOINT_ID;
  signal covered : std_logic := '0';
begin

  ins_ready  <= outs_ready;
  outs_valid <= ins_valid;
  outs       <= ins;

  process (clk, rst)
  begin
    if rst = '1' then
      covered <= '0';
    elsif rising_edge(clk) then
      if outs_valid = '1' and outs_ready = '1' then
        covered <= '1';
      end if;
    end if;
  end process;

end architecture arch;
