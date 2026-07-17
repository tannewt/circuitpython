# GBIO Architecture — Address & Data Flow

```mermaid
flowchart TB
    subgraph GB["🎮 Game Boy Cartridge Bus"]
        A0_15["A0..A15 (GPIO2..17)"]
        D0_7["D0..D7 (GPIO23..30)"]
        nCS["/CS (GPIO21)"]
        nWR["/WR (GPIO19)"]
        nRD["/RD (GPIO20)"]
        CLK["CLK (GPIO18)"]
        nRESET["/RESET (GPIO31)"]
    end

    subgraph PIO0["PIO0 — 4 State Machines"]
        subgraph Monitor["Monitor SMs (shared program)"]
            SM_CS["SM: monitor_cs<br/>jmp pin = /CS (GPIO21)<br/>pin↓ → IRQ0 clear<br/>pin↑ → IRQ0 set"]
            SM_A15["SM: monitor_a15<br/>jmp pin = A15 (GPIO17)<br/>pin↓ → IRQ0 clear<br/>pin↑ → IRQ0 set"]
        end
        SM_ADDR["SM: address<br/>waits IRQ0 cleared<br/>in pins = A0..A15<br/>captures 16-bit addr → RX FIFO"]
        SM_OUT["SM: output<br/>waits IRQ0 cleared<br/>waits CLK low<br/>jmp pin = /WR<br/>READ:  out D0..D7 from TX FIFO<br/>WRITE: in  D0..D7 → RX FIFO<br/>sideset = DATA_OE (GPIO22)"]
    end

    subgraph PIO1["PIO1 — Debug SM"]
        SM_DBG["SM: debug<br/>waits A15↓<br/>in pins = GPIO0..31<br/>samples 32-bit snapshot → RX FIFO<br/>waits CLK↓, samples again"]
    end

    subgraph DMA["DMA Channels (6 + debug)"]
        direction LR
        DMA_ADDR["dma_addr_chan<br/>16-bit transfers<br/>RX FIFO → circular addr buf<br/>🔍 SNIFFER watches this<br/>(sum mode: base + addr)<br/>chains→ sniff_read"]
        DMA_SNIFF_R["dma_sniff_read_chan<br/>32-bit transfer<br/>sniff_data → read_addr reg<br/>of dma_data_read_chan<br/>chains→ sniff_write"]
        DMA_READ["dma_data_read_chan<br/>8-bit transfers<br/>data_buf[addr] → TX FIFO<br/>(read addr set by sniff_read)"]
        DMA_SNIFF_W["dma_sniff_write_chan<br/>32-bit transfer<br/>sniff_data → write_addr reg<br/>of dma_data_write_chan<br/>chains→ sniff_reset"]
        DMA_WRITE["dma_data_write_chan<br/>8-bit transfers<br/>RX FIFO → data_buf[addr]<br/>(write addr set by sniff_write)<br/>independent, DREQ-driven"]
        DMA_RESET["dma_sniff_reset_chan<br/>32-bit transfer<br/>resets sniffer accumulator<br/>back to buffer_base<br/>chains→ dma_addr_chan 🔁"]
        DMA_DBG["debug_dma_chan<br/>32-bit transfers<br/>debug RX FIFO → debug_samples[]"]
    end

    subgraph MEM["RP2350 RAM"]
        BUF64K["gb_data_buffer[65536]<br/>64 KB flat address space<br/>ARM writes SM83 opcodes here"]
        ADDR_BUF["gb_address_buffer[1024]<br/>circular log of accessed addrs"]
        DBG_BUF["debug_samples[]<br/>GPIO snapshots for analysis"]
    end

    subgraph ARM["ARM Cortex-M33 Core"]
        PYTHON["CircuitPython / USB<br/>writes opcodes into buffer<br/>reads button state from buffer<br/>polls vsync counter"]
    end

    %% Game Boy → PIO connections
    nCS --> SM_CS
    A0_15 --> SM_A15
    A0_15 --> SM_ADDR
    A0_15 --> SM_DBG
    D0_7 <--> SM_OUT
    nWR --> SM_OUT
    CLK --> SM_OUT
    nRD --> SM_DBG

    %% IRQ signaling
    SM_CS -- "IRQ0 clear/set" --> SM_ADDR
    SM_A15 -- "IRQ0 clear/set" --> SM_ADDR
    SM_CS -- "IRQ0 clear/set" --> SM_OUT
    SM_A15 -- "IRQ0 clear/set" --> SM_OUT

    %% Address flow: SM → DMA → sniffer
    SM_ADDR -- "RX FIFO (16-bit addr)" --> DMA_ADDR
    DMA_ADDR -- "writes to" --> ADDR_BUF
    DMA_ADDR -- "sniffer computes<br/>buffer_base + addr" --> DMA_SNIFF_R

    %% Read path
    DMA_SNIFF_R -- "sets read address" --> DMA_READ
    DMA_READ -- "reads byte from" --> BUF64K
    DMA_READ -- "TX FIFO (8-bit data)" --> SM_OUT

    %% Write path
    DMA_SNIFF_R --> DMA_SNIFF_W
    DMA_SNIFF_W -- "sets write address" --> DMA_WRITE
    SM_OUT -- "RX FIFO (8-bit data)" --> DMA_WRITE
    DMA_WRITE -- "writes byte to" --> BUF64K

    %% Reset chain
    DMA_SNIFF_W --> DMA_RESET
    DMA_RESET -- "resets sniffer → buffer_base" --> DMA_ADDR

    %% Debug path
    SM_DBG -- "RX FIFO (32-bit)" --> DMA_DBG
    DMA_DBG --> DBG_BUF

    %% ARM ↔ buffer
    PYTHON -- "read/write" --> BUF64K
    PYTHON -- "read debug" --> DBG_BUF

    %% Styling
    style BUF64K fill:#2d5016,stroke:#4a7,color:#fff
    style ADDR_BUF fill:#2d5016,stroke:#4a7,color:#fff
    style DBG_BUF fill:#2d5016,stroke:#4a7,color:#fff
    style DMA_ADDR fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_SNIFF_R fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_READ fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_SNIFF_W fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_WRITE fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_RESET fill:#1a3a5c,stroke:#48f,color:#fff
    style DMA_DBG fill:#1a3a5c,stroke:#48f,color:#fff
    style SM_CS fill:#5c1a3a,stroke:#f48,color:#fff
    style SM_A15 fill:#5c1a3a,stroke:#f48,color:#fff
    style SM_ADDR fill:#5c1a3a,stroke:#f48,color:#fff
    style SM_OUT fill:#5c1a3a,stroke:#f48,color:#fff
    style SM_DBG fill:#5c1a3a,stroke:#f48,color:#fff
```

## How It Works

The RP2350 acts as a **Game Boy cartridge**. A 64 KB buffer in RAM (`gb_data_buffer`) maps the entire Game Boy address space. PIO state machines and DMA serve reads/writes automatically — the ARM core is free to run Python/USB.

### Read Cycle (Game Boy reads from cartridge)

| Step | Who | What |
|------|-----|------|
| 1 | **Monitor CS SM** | /CS goes low → clears PIO IRQ0 |
| 2 | **Address SM** | Sees IRQ0 cleared, captures A0..A15 into RX FIFO |
| 3 | **dma_addr_chan** | Copies 16-bit address from RX FIFO → circular buffer. **Sniffer** (in sum mode, seeded with `buffer_base`) computes `buffer_base + address` |
| 4 | **dma_sniff_read_chan** | Copies sniffer result into `dma_data_read_chan`'s **read-address register** (chains from step 3) |
| 5 | **dma_data_read_chan** | Reads byte from `gb_data_buffer[address]` → Output SM's **TX FIFO** |
| 6 | **Output SM** | Waits for CLK low, sees /WR high (read), outputs byte on D0..D7 with DATA_OE asserted |
| 7 | **dma_sniff_write_chan** | Copies sniffer into `dma_data_write_chan`'s write-address register (chains from step 4) |
| 8 | **dma_sniff_reset_chan** | Resets sniffer accumulator back to `buffer_base` (chains from step 7) |
| 9 | 🔁 chains back to **dma_addr_chan** — ready for next access |

### Write Cycle (Game Boy writes to cartridge)

Same as read through step 4, then:

| Step | Who | What |
|------|-----|------|
| 5w | **dma_sniff_write_chan** | Sets `dma_data_write_chan`'s write address to `buffer_base + address` |
| 6w | **Output SM** | Sees /WR low (write), captures D0..D7 into **RX FIFO** |
| 7w | **dma_data_write_chan** | Copies byte from RX FIFO → `gb_data_buffer[address]` (independent channel, DREQ-driven) |

### Key Design Details

- **Sniffer trick**: The DMA sniffer in "sum mode" acts as an address adder. Seeded with the buffer base pointer, adding the 16-bit address yields the exact buffer offset — no CPU intervention needed.
- **Two monitors**: Both /CS and A15 are monitored because the Game Boy can assert either to start an access. They share one PIO program but use different `jmp_pin` configs.
- **DATA_OE sideset**: The output SM controls the level-shifter's output-enable via a sideset pin, ensuring the RP2350 only drives the data bus during reads (and releases it for writes).
- **Debug PIO**: A separate PIO1 SM captures full 32-bit GPIO snapshots on every A15 falling edge + CLK low, DMA'd to a buffer for post-mortem analysis.

### DMA Chain Order

```
dma_addr_chan → dma_sniff_read_chan → dma_sniff_write_chan → dma_sniff_reset_chan → dma_addr_chan (loop)
                    |                         |
                    v                         v
            dma_data_read_chan        dma_data_write_chan
            (read addr updated)       (write addr updated)
```

`dma_data_read_chan` and `dma_data_write_chan` are **not** in the chain — they are triggered by PIO DREQ signals (TX FIFO empty / RX FIFO not empty) and their address registers are updated by the sniffer channels.

### Pin Map (PyGameBoy RP2350 v8 PCB)

| Signal | GPIO | Direction |
|--------|------|-----------|
| A0..A15 | 2..17 | Input (SIO) |
| CLK | 18 | Input |
| /WR | 19 | Input |
| /RD | 20 | Input |
| /CS | 21 | Input |
| DATA_OE | 22 | Output (PIO sideset) |
| D0..D7 | 23..30 | Bidirectional (PIO) |
| /GB_RESET | 31 | Output (GPIO) |
| DEBUG_LED | 45 | Output (GPIO) |
