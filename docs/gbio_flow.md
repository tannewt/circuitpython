# GBIO Address & Data Flow Architecture

```mermaid
flowchart LR
    subgraph GB["Game Boy"]
        GB_BUS["Cartridge Bus<br/>A0..A15, D0..D7, /CS, /WR, CLK"]
    end

    subgraph PIO["PIO0 State Machines"]
        MON_CS["Monitor CS<br/>watches /CS → IRQ 0"]
        MON_A15["Monitor A15<br/>watches A15 → IRQ 0"]
        ADDR["Address SM<br/>captures A0..A15"]
        DATA["Output SM<br/>reads D0..D7 out<br/>writes D0..D7 in"]
    end

    subgraph DMA["DMA Sniffer Chain"]
        direction LR
        A["addr DMA<br/>address → sniffer"]
        SN["sniffer<br/>buffer_ptr + addr"]
        SR["sniff_read<br/>sniffer → read DMA addr"]
        DR["data_read DMA<br/>buffer → TX FIFO"]
        SW["sniff_write<br/>sniffer → write DMA addr"]
        DW["data_write DMA<br/>RX FIFO → buffer"]
        RS["sniff_reset<br/>reset sniffer → loop"]
    end

    subgraph BUF["64K Buffer"]
        B["gb_data_buffer[65536]<br/>maps GB address space"]
    end

    GB_BUS --> MON_CS
    GB_BUS --> MON_A15
    MON_CS --> IRQ0((IRQ 0))
    MON_A15 --> IRQ0
    IRQ0 --> ADDR
    IRQ0 --> DATA
    GB_BUS -->|"A0..A15"| ADDR

    ADDR -->|"address"| A
    A --> SN
    SN --> SR
    SR --> DR
    DR -->|"data byte"| DATA
    DATA -->|"D0..D7 out"| GB_BUS
    GB_BUS -->|"D0..D7 in"| DATA

    SN --> SW
    SW --> DW
    DATA -->|"captured byte"| DW
    DW --> B

    B --> DR
    RS --> SN
    RS -.->|"chain loop"| A

    ARM["ARM Core<br/>writes opcodes,<br/>reads vsync/gamepad"] --> B
```

## How it works

1. **Monitor SMs** watch `/CS` and `A15`. When either goes low, they clear IRQ 0 — signaling "an access is starting."

2. **Address SM** wakes up on IRQ 0 cleared, captures the 16-bit address from A0..A15 into its RX FIFO.

3. **DMA chain** processes the address:
   - `addr DMA` sends the address into the **sniffer**, which adds it to the buffer base pointer (`buffer_ptr + address`)
   - For **reads**: `sniff_read` copies the sniffer result into `data_read`'s source address → `data_read` fetches the byte from the buffer and pushes it into the Output SM's TX FIFO → Output SM drives D0..D7 onto the bus
   - For **writes**: `sniff_write` copies the sniffer result into `data_write`'s destination address → Output SM captures D0..D7 into its RX FIFO → `data_write` stores it in the buffer at the right offset
   - `sniff_reset` resets the sniffer back to the buffer base, and the chain loops

4. **64K buffer** is the shared memory: the ARM core writes SM83 opcodes here, and the PIO/DMA hardware serves bytes automatically on every Game Boy access.
